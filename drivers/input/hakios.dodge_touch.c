/*
 * The touch screen of the OnePlus 13 (dodge, SM8750): a Synaptics S3910 on SPI, as a source of
 * touch records for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg synaptics=1.
 *
 * SEEN WORKING on the phone (October 2026): tap, drag, multitouch and edge gestures at 125 Hz,
 * through a chain this driver builds itself, none of which the boot loaders leave standing:
 *   - the SPI bus is GPIO 48/49/50/51 of the TLMM, bit-banged (mode 0, MSB first): the qup engine
 *     needs GENI firmware and clock votes this VM cannot have;
 *   - the panel's AVDD is GPIO3 of the PM8550VS (SPMI page 0x8a, ppid 0x98a), powered through the
 *     arbiter's write side (the 4 MB "chnls" region, opcode 0: the only write path that reaches
 *     hardware under this hypervisor -- the observer's writes land in Gunyah's cache);
 *   - VDD_IO 1.8V is ldo.b4 of the PM8550, voted through the apps RSC drv@2 at 0x16520000
 *     (an idle ACTIVE TCS of the three, command addresses from the AOP command database at
 *     0x81c60000), everything restored after the vote;
 *   - the protocol is Synaptics TouchCom, ported from the OnePlus kernel's
 *     synaptics_touchcom_func_touch.c: the report stream is configured with the FIRMWARE'S OWN
 *     default config (GET_TOUCH_REPORT_CONFIG is read and kept, nothing is written into the chip's
 *     configuration), which is how Android's recovery gets touch with no firmware file at all.
 *
 * The report config interpreter ("s39_decode_frame") is that generic TouchCom interpreter; the
 * hardware layer below ("the hardware layer" section) was hakios.dodge_touch_hw.h, a device driver
 * being one file (docs/porting/drivers.md).
 *
 * Addresses (the phone's device tree and the OnePlus SM8750 kernel):
 *   TLMM          0x0f100000, 0x1000 per pin
 *   apps rsc drv2 0x16520000 (tcs-offset 0xd00, 0x2a0 per TCS -- SM8750's v3 layout, 24-byte commands)
 *   aop cmd-db    0x81c60000
 *   spmi          core 0x0c400000, observer 0x0c440000 (0x20 per APID), chnls 0x0c500000 (0x1000 per APID)
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

static uint32_t s39_rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static void s39_wr(uintptr_t a, uint32_t v) {
    *(volatile uint32_t *)a=v; __asm__ volatile("dsb sy" ::: "memory");
}
static void s39_delay(unsigned us) { delay_for_interval(us, NSEC_PER_USEC); }

/* OnePlus 13 / dodge S3910 transport. Ported from the proven Ace 5 (giulia)
 * implementation with three dodge-specific changes:
 *   - APPS RSC drv@2 at 0x16520000 (SM8750; giulia used 0x17a20000 on SM8650).
 *     tcs-offset 0xd00 / slot stride 0x2a0 confirmed by the live Android DT.
 *   - AVDD is PM8550VS GPIO3 via SPMI (opcode 0 EXT_WRITEL on the RW channel,
 *     proven from this kernel), not a TLMM pin like giulia's pin 108.
 *   - VDD_IO 1.8V is ldo.b4 (pm_humu_l4), voted through the same RSC TCS.
 * The includer provides s39_rd, s39_wr and s39_delay (microseconds).
 */
/* ---- the hardware layer (was hakios.dodge_touch_hw.h; a device driver is one file: docs/porting/drivers.md) ---- */

#define S39_TLMM_PA 0x0f100000ULL /* TLMM per-pin register bank */
#define S39_RSC_PA  0x16520000ULL /* apps rsc drv@2 */
#define S39_DB_PA   0x81c60000ULL /* aop cmd-db */
#define S39_SPMI_CORE_PA 0x0c400000ULL
#define S39_SPMI_OBS_PA  0x0c440000ULL
#define S39_SPMI_CHN_PA  0x0c500000ULL
#define S39_MAX 2048
struct s39_hw {
    uintptr_t gpio, rsc, db, spmi_core, spmi_obs, spmi_chn;
    uint32_t regulator, rsc_id, last_status, slot;
    uint8_t header[4], data[S39_MAX + 3];
    unsigned length, max_x, max_y, setup_step;
};
static uintptr_t s39_cfg(struct s39_hw *h, unsigned pin) { return h->gpio + 0x1000 * pin; }
static void s39_set(struct s39_hw *h, unsigned pin, unsigned high) {
    s39_wr(s39_cfg(h, pin) + 4, high ? 2 : 0);
}
static unsigned s39_get(struct s39_hw *h, unsigned pin) {
    return s39_rd(s39_cfg(h, pin) + 4) & 1;
}
static void s39_pin(struct s39_hw *h, unsigned pin, unsigned output, unsigned high) {
    if (output) s39_set(h, pin, high);
    /* Preserve eGPIO/security bits; mux GPIO, no pull, 6 mA. */
    uint32_t v = s39_rd(s39_cfg(h, pin));
    s39_wr(s39_cfg(h, pin), (v & ~0x3ffu) | (2 << 6) | (output << 9));
}
static uint16_t s39_le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t s39_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t s39_find_ldob4(struct s39_hw *h) {
    const uint8_t *db = (const uint8_t *)h->db;
    if (s39_le32(db + 4) != 0x0c0330db) return 0;
    for (unsigned s = 0; s < 8; s++) {
        const uint8_t *d = db + 8 + s * 16;
        unsigned off = s39_le16(d + 2), n = s39_le16(d + 6);
        if (!s39_le16(d) || n > 512 || off + n * 24 > 0x20000 - 144) continue;
        for (unsigned i = 0; i < n; i++) {
            const uint8_t *e = db + 144 + off + 24 * i;
            if (e[0]=='l' && e[1]=='d' && e[2]=='o' && e[3]=='b' && e[4]=='4' && !e[5] && !e[6] && !e[7])
                return s39_le32(e + 16);
        }
    }
    return 0;
}
/* PM8550VS GPIO3 = AVDD 2.8V. ALL access goes through the OBSERVER channel
 * (0x0c440000 + 0x20*apid): this is the path UEFI proved reaches the real PMIC
 * (opcode 2 write / opcode 13 read). The 0x0c500000 RW channel is emulated by
 * Gunyah in this VM — writes land in a cache, readback lies. */
static int s39_obs_read(struct s39_hw *h, int apid, uint8_t off, uint8_t *val)
{
    volatile uint32_t *o = (volatile uint32_t *)(h->spmi_obs + 0x20 * apid);
    o[0] = (13u << 27) | ((uint32_t)off << 4) | 0u;
    uint32_t st = 0;
    for (int t = 0; t < 2000000; t++) { st = o[2]; if (st & 1) break; }
    if ((st & 1) && !(st & 0xe)) { *val = o[6] & 0xff; return 0; }
    return -(int)(st & 0xf);
}
static int s39_chwn(struct s39_hw *h, int apid, uint32_t opc, uint8_t off, uint8_t val)
{
    volatile uint32_t *w = (volatile uint32_t *)(h->spmi_chn + 0x1000 * apid);
    w[4] = val;                                   /* WDATA +0x10 */
    w[0] = (opc << 27) | ((uint32_t)off << 4);    /* CMD +0x00 */
    for (int t = 0; t < 1000000; t++) { if (w[2] & 1) return 0; }  /* STATUS +0x08 */
    return -1;
}
static int s39_avdd(struct s39_hw *h)
{
    uintptr_t core = h->spmi_core;
    unsigned cnt = s39_rd(core + 4) & 0x7ff;
    int apid = -1;
    for (unsigned i = 0; i < cnt && i < 900; i++) {
        uint32_t v = s39_rd(core + 0x2000 + 4*i);
        if (v && ((v >> 8) & 0xfff) == 0x98a) { apid = (int)i; break; }
    }
    if (apid < 0) return -1;
    h->last_status = (uint32_t)apid;
    /* Touch AVDD = GPIO3 = ppid 0x98a (page 0x8a00). DT gpio number 3 is the
     * 0-indexed pin 2 (0x8800 + 2*0x100) — 0x8bxx is GPIO4! Android's exact
     * live values (regmap debugfs, touch working): mode=digital-out (0x02),
     * vin-1, pull 4, out source HIGH (0x80), enabled (0x80). Written through
     * the chnls channel with opcode 0, the only write path that reaches
     * hardware in this VM.
     * Full power cycle first (Android's ops_power_on sequence): the chip has
     * been continuously powered since XBL — without one, the firmware's
     * report engine never starts. */
    (void)s39_chwn(h, apid, 0, 0x46, 0x00);   /* AVDD off */
    s39_delay(100000);
    static const struct { uint8_t off, val; } seq[] = {
        {0x40, 0x02}, {0x41, 0x01}, {0x42, 0x04}, {0x43, 0x00},
        {0x44, 0x80}, {0x45, 0x01}, {0x46, 0x80},
    };
    for (unsigned i = 0; i < sizeof(seq)/sizeof(seq[0]); i++)
        (void)s39_chwn(h, apid, 0, seq[i].off, seq[i].val);
    uint8_t post[7];
    for (unsigned i = 0; i < 7; i++)
        s39_obs_read(h, apid, (uint8_t)(0x40 + i), &post[i]);
    printf("s3910: avdd gpio3(0x98a) real: 40=%02x 41=%02x 42=%02x 43=%02x 44=%02x 45=%02x 46=%02x\n",
           post[0], post[1], post[2], post[3], post[4], post[5], post[6]);
    return 0;
}
/* one 3-command AMC vote through an idle ACTIVE TCS slot (proven format) */
static int s39_tcs_vote(struct s39_hw *h, const uint32_t addrs[3], const uint32_t values[3])
{
    uintptr_t base = h->rsc + 0xd00, t = 0;
    for (unsigned slot=0; slot<3; slot++) {
        uintptr_t candidate=base+slot*672;
        if (s39_rd(candidate+0x28) && !(s39_rd(candidate+0x24)&0x1000000) && !s39_rd(candidate+0x2c)) {
            t=candidate; h->slot=slot; break;
        }
    }
    if (!t) return -12;
    uint32_t mask=1u<<h->slot, irq = s39_rd(base);
    uint32_t control=s39_rd(t+0x24), wait=s39_rd(t+0x20);
    s39_wr(t+0x24, control & ~0x10000u);
    uint32_t saved[9];
    for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 3; j++)
        saved[i*3+j] = s39_rd(t + 0x34 + i*24 + j*4);
    s39_wr(base + 8, mask);
    for (unsigned i = 0; i < 3; i++) {
        s39_wr(t + 0x34 + i*24, 0x10108);
        s39_wr(t + 0x38 + i*24, addrs[i]);
        s39_wr(t + 0x3c + i*24, values[i]);
    }
    s39_wr(t + 0x20, 7); s39_wr(t + 0x2c, 7); s39_wr(base, irq | mask);
    s39_wr(t + 0x24, 0x10000); s39_wr(t + 0x24, 0x1010000);
    unsigned i;
    for (i=0; i<2000 && !(s39_rd(base+4)&mask); i++) s39_delay(100);
    h->last_status = s39_rd(base+4);
    if (i == 2000) return -13;
    s39_wr(t+0x24, 0x10000); s39_wr(t+0x24, 0);
    s39_wr(t+0x2c, 0); s39_wr(t+0x20, wait); s39_wr(base+8, mask); s39_wr(base, irq);
    for (unsigned a=0; a<3; a++) for (unsigned j=0; j<3; j++)
        s39_wr(t + 0x34 + a*24 + j*4, saved[a*3+j]);
    s39_wr(t+0x24, control);
    return 0;
}

static void s39_cmddb_dump(struct s39_hw *h) {
    const uint8_t *db = (const uint8_t *)h->db;
    unsigned shown = 0;
    for (unsigned s = 0; s < 8; s++) {
        const uint8_t *d = db + 8 + s * 16;
        unsigned off = s39_le16(d + 2), n = s39_le16(d + 6);
        if (!s39_le16(d) || n > 512 || off + n * 24 > 0x20000 - 144) continue;
        for (unsigned i = 0; i < n && shown < 72; i++) {
            const uint8_t *e = db + 144 + off + 24 * i;
            if (e[0] < 32 || e[0] > 127) continue;
            char name[9];
            for (unsigned k = 0; k < 8; k++) name[k] = e[k] ? e[k] : ' ';
            name[8] = 0;
            printf("s3910: cmddb %s %08x\n", name, s39_le32(e + 16));
            shown++;
        }
    }
}

static int s39_power(struct s39_hw *h) {
    h->regulator = s39_find_ldob4(h);
    h->rsc_id = s39_rd(h->rsc);
    if (!h->regulator) return -10;
    /* ldo.b4 (VDD_IO 1.8V): voltage, enable, HPM */
    uint32_t la[3] = { h->regulator + 0, h->regulator + 4, h->regulator + 8 };
    uint32_t lv[3] = { 1800, 1, 4 };
    int rc = s39_tcs_vote(h, la, lv);
    printf("s3910: vote ldo.b4 rc=%d status=%#x\n", rc, h->last_status);
    if (rc) return rc;
    s39_avdd(h);   /* write-path experiment with real readback */
    s39_delay(200000);
    return 0;
}

static void s39_bus_init(struct s39_hw *h) {
    s39_pin(h, 51, 1, 1); /* CS inactive first */
    s39_pin(h, 50, 1, 0); s39_pin(h, 49, 1, 1); s39_pin(h, 48, 0, 0);
    s39_pin(h, 162, 0, 0);
}
static void s39_reset(struct s39_hw *h) {
    s39_pin(h, 161, 1, 0); s39_delay(10000);
    s39_set(h, 161, 1); s39_delay(80000);
}
/* Mode 0, MSB first. Conservative bit-bang transport avoids GENI firmware/clock
 * assumptions. Reading clocks out 0xff, as the original Linux transport does. */
static void s39_spi(struct s39_hw *h, const uint8_t *tx, uint8_t *rx, unsigned n) {
    s39_set(h, 51, 0); s39_delay(2);
    for (unsigned i=0; i<n; i++) {
        uint8_t v = tx ? tx[i] : 0xff, r = 0;
        for (unsigned b=0; b<8; b++) {
            s39_set(h, 49, (v >> (7-b)) & 1);
            s39_set(h, 50, 1);
            r = (r << 1) | s39_get(h, 48);
            s39_set(h, 50, 0);
        }
        if (rx) rx[i] = r;
    }
    s39_delay(2); s39_set(h, 51, 1); s39_delay(10);
}
/* Return report/status code, or a negative transport error. */
static int s39_read(struct s39_hw *h) {
    h->length = 0;
    s39_spi(h, 0, h->header, 4);
    if (h->header[0] != 0xa5) return -20;
    unsigned n = s39_le16(h->header+2);
    if (h->header[1] == 0 || h->header[1] == 0xff) return 0;
    if (n > S39_MAX) return -21;
    if (n) {
        s39_spi(h, 0, h->data, n+3);
        if (h->data[0]!=0xa5 || h->data[1]!=3 || h->data[n+2]!=0x5a) return -22;
        for (unsigned i=0; i<n; i++) h->data[i] = h->data[i+2];
    }
    h->length = n;
    return h->header[1];
}
static int s39_command(struct s39_hw *h, uint8_t cmd, const uint8_t *p, unsigned n) {
    uint8_t out[259];
    if (n > 256) return -23;
    out[0]=cmd; out[1]=n; out[2]=n>>8;
    for (unsigned i=0; i<n; i++) out[i+3]=p[i];
    s39_spi(h, out, 0, n+3);
    for (unsigned i=0; i<100; i++) {
        s39_delay(2000);
        if (s39_get(h, 162)) continue;
        int r = s39_read(h);
        if (r < 0 || (r > 0 && r < 0x10)) return r;
    }
    return -24;
}
/* Configure a compact volatile report: count, then {index:4,class:4,x:16,y:16}.
 * No configuration commit, firmware update or nonvolatile write is issued. */
static const uint8_t s39_report_config[] = {0x18,8, 1, 6,4, 7,4, 8,16, 9,16, 3,0};
static int s39_start_reports(struct s39_hw *h) {
    uint8_t p[256]={0}; int r;
    h->setup_step=1;
    if ((r=s39_command(h,0x20,0,0)) != 1) return r;
    if (h->length<40 || s39_le16(h->data+2)) return -30;
    h->max_x=s39_le16(h->data+32); h->max_y=s39_le16(h->data+34);
    unsigned size=s39_le16(h->data+12);
    if (!h->max_x || !h->max_y || size<sizeof(s39_report_config) || size>sizeof(p)) return -31;
    h->setup_step=2; p[0]=0x23;
    if ((r=s39_command(h,6,p,1)) != 1) return r; /* stop HBP frames */
    h->setup_step=3; p[0]=0x11; p[1]=2; p[2]=0;
    if ((r=s39_command(h,0x24,p,3)) != 1) return r; /* LBP */
    h->setup_step=4; p[0]=9; p[1]=0;
    if ((r=s39_command(h,0x24,p,3)) != 1) return r; /* normal sensing */
    h->setup_step=5;
    for (unsigned i=0;i<size;i++) p[i]=i<sizeof(s39_report_config)?s39_report_config[i]:0;
    if ((r=s39_command(h,0x26,p,size)) != 1) return r;
    h->setup_step=6;
    if ((r=s39_command(h,0x25,0,0)) != 1) return r;
    if (h->length<sizeof(s39_report_config)) return -32;
    for (unsigned i=0;i<sizeof(s39_report_config);i++) if (h->data[i]!=s39_report_config[i]) return -32;
    h->setup_step=7; p[0]=0x11;
    if ((r=s39_command(h,5,p,1)) != 1) return r;
    h->setup_step=8;
    return 0;
}


static struct s39_hw hw;

/* ---- generic report config interpreter (TouchComm) ---- */
#define S39_MAX_CFG 256
static uint8_t rep_cfg[S39_MAX_CFG];
static unsigned rep_cfg_len;
static unsigned fw_max_x, fw_max_y, fw_max_objects;

struct s39_obj { unsigned id, cls, x, y; };

/* LSB-first bit extraction, as the reference driver */
static unsigned getbits(const uint8_t *rep, unsigned rep_bits, unsigned off, unsigned bits)
{
    if (!bits || bits > 32 || off + bits > rep_bits) return 0;
    unsigned out = 0, rem = bits, bo = off % 8, by = off / 8;
    while (rem) {
        unsigned d = rep[by] >> bo;
        unsigned avail = 8 - bo, take = avail < rem ? avail : rem;
        d &= 0xffu >> (8 - take);
        out |= d << (bits - rem);
        bo = 0; by++; rem -= take;
    }
    return out;
}

static void commit_obj(struct s39_obj cur, struct s39_obj objs[10], unsigned *mask, unsigned *seen)
{
    if (cur.id >= 10 || (*seen & (1u << cur.id))) return;
    *seen |= 1u << cur.id;
    /* finger(1), gloved(2), stylus(3), small object(5), EDGE TOUCHED(8) count;
     * lift(0), palm(6), hover(9) do not — edge touches are the home gesture! */
    if (cur.cls == 1 || cur.cls == 2 || cur.cls == 3 || cur.cls == 5 || cur.cls == 8) {
        objs[cur.id].cls = cur.cls;
        objs[cur.id].x = cur.x;
        objs[cur.id].y = cur.y;
        *mask |= 1u << cur.id;
    }
}

/* Parse one touch report per the firmware's own config. Returns 0 on a
 * well-formed frame; fills objs[] (normalized to 65535) and mask. */
static int s39_decode_frame(const uint8_t *rep, unsigned rep_len,
                            struct s39_obj objs[10], unsigned *mask)
{
    if (!rep_cfg_len) return -1;
    unsigned rep_bits = rep_len * 8;
    unsigned off = 0, num_active = 0, have_num = 0;
    unsigned seen = 0, idx = 0, loop_next = 0, objects = 0;
    struct s39_obj cur = {0,0,0};
    int cur_valid = 0;
    *mask = 0;
    memset(objs, 0, 10 * sizeof(*objs));

    while (idx < rep_cfg_len) {
        unsigned code = rep_cfg[idx++];
        switch (code) {
        case 0x00: /* END */
            goto done;
        case 0x01: /* FOREACH_ACTIVE_OBJECT */
        case 0x02: /* FOREACH_OBJECT */
            loop_next = idx; objects = 0; cur_valid = 0;
            break;
        case 0x03: /* FOREACH_END */
            if (cur_valid) { commit_obj(cur, objs, mask, &seen); cur_valid = 0; }
            objects++;
            if ((have_num && objects < num_active) || (!have_num && off < rep_bits))
                idx = loop_next;
            break;
        case 0x04: /* PAD_TO_NEXT_BYTE */
            off = (off + 7) & ~7u;
            break;
        default: { /* entity codes carry one length byte */
            if (idx >= rep_cfg_len) goto done;
            unsigned bits = rep_cfg[idx++];
            unsigned v = getbits(rep, rep_bits, off, bits);
            switch (code) {
            case 0x18: num_active = v; have_num = 1; break;   /* num active objects */
            case 0x06: cur.id = v; cur_valid = 1; break;      /* object index */
            case 0x07: cur.cls = v; break;                    /* classification */
            case 0x08: cur.x = v; break;                      /* x position */
            case 0x09: cur.y = v; break;                      /* y position */
            default: break;                                   /* timestamp, z, widths... */
            }
            off += bits;
            break; }
        }
    }
done:
    if (cur_valid) commit_obj(cur, objs, mask, &seen);
    /* clamp to the panel: a bottom-edge finger can report y == max (+ a hair) */
    for (unsigned i = 0; i < 10; i++)
        if (*mask & (1u << i)) {
            if (objs[i].x > fw_max_x) objs[i].x = fw_max_x;
            if (objs[i].y > fw_max_y) objs[i].y = fw_max_y;
        }
    return 0;
}

static struct s39_obj last[10];
static unsigned active;

static void release_all(void) {
    for (unsigned i=0;i<10;i++) if (active&(1u<<i))
        hakios_touch_post(HAKIOS_TOUCH_UP,i,
            (uint32_t)last[i].x*65535/(fw_max_x?fw_max_x:1),
            (uint32_t)last[i].y*65535/(fw_max_y?fw_max_y:1));
    active=0;
}

/* (Re)start the report stream: the whole proven recipe after any reset. */
static int s39_bringup(struct s39_hw *h)
{
    int rc;
    rc=s39_command(h,0x2d,0,0); /* CMD_EXIT_DEEP_SLEEP */
    if (rc!=1) printf("s3910: exit deep sleep rc=%d\n",rc);
    rc=s39_command(h,0x27,0,0); /* CMD_REZERO */
    rc=s39_command(h,0x20,0,0); /* GET_APP_INFO (also proves the fw answers) */
    if (rc!=1 || h->length<38) return -1;
    if (!fw_max_x) {
        fw_max_x=s39_le16(h->data+32); fw_max_y=s39_le16(h->data+34);
        printf("s3910: app status=%#x max=%u,%u objs=%u cfgid=%.16s\n",
            s39_le16(h->data+2),fw_max_x,fw_max_y,s39_le16(h->data+36),(const char *)h->data+16);
    }
    rc=s39_command(h,0x25,0,0); /* GET_TOUCH_REPORT_CONFIG (preserve the fw default) */
    if (rc==1 && h->length && h->length<=S39_MAX_CFG && !rep_cfg_len) {
        rep_cfg_len=h->length; memcpy(rep_cfg,h->data,rep_cfg_len);
        printf("s3910: report config kept (%u bytes)\n",rep_cfg_len);
    }
    { uint8_t q[3]={0x01,1,0}; (void)s39_command(h,0x24,q,3); } /* DC_DISABLE_DOZE */
    { uint8_t q[3]={0xd4,0,0}; (void)s39_command(h,0x24,q,3); } /* touch-and-hold off */
    { uint8_t q[3]={0xfe,0,0}; (void)s39_command(h,0x24,q,3); } /* gestures off */
    { uint8_t q[1]={0x11}; rc=s39_command(h,0x05,q,1); }        /* enable REPORT_TOUCH */
    if (rc!=1) return -2;
    { uint8_t q[3]={0xf2,150,0}; (void)s39_command(h,0x24,q,3); } /* 150 Hz */
    { uint8_t q[3]={0xf5,150,0}; (void)s39_command(h,0x24,q,3); }
    return 0;
}

/* Ask the fw why it went quiet BEFORE resetting it: app status, the frame-rate
 * DCs (did a lease expire?), the protocol-mode DC. (cmd 0x23 = GET_DYNAMIC_CONFIG,
 * payload = the DC id -- that is also what the daemon's "0x23 {ff}" was.) */
static void s39_stall_diag(struct s39_hw *h)
{
    int rc=s39_command(h,0x20,0,0);            /* GET_APP_INFO */
    unsigned st = (rc==1 && h->length>=4) ? s39_le16(h->data+2) : 0xdead;
    printf("s3910: stall diag: app status=%#x",st);
    static const uint8_t ids[]={0xf2,0xf5,0x11,0x01,0xd4};
    for (unsigned i=0;i<sizeof(ids);i++) {
        rc=s39_command(h,0x23,(const uint8_t *)&ids[i],1);   /* GET_DYNAMIC_CONFIG */
        printf(" dc%02x=",ids[i]);
        if (rc==1 && h->length>=2) printf("%u",s39_le16(h->data));
        else printf("?");
    }
    printf("\n");
}

/* Full recovery after the stream stalls or the fw reboots itself: lift every
 * finger (a stuck touch freezes iOS into its press-and-hold state), reset the
 * chip and bring the stream up again. */
static void s39_recover(struct s39_hw *h)
{
    printf("s3910: stream stalled -- recovering\n");
    s39_stall_diag(h);
    release_all();
    s39_bus_init(h);
    s39_reset(h);
    {   /* the boot-time detect, verbatim: the 0x07 bus magic resyncs the fw's
         * protocol state after every reset (without it a reset chip never
         * reports again) */
        uint8_t bare[4];
        uint8_t magic[1]={0x07};
        s39_spi(h,magic,0,1);
        s39_delay(100);
        s39_spi(h,0,bare,4);
        printf("s3910: detect bare: %02x %02x %02x %02x\n",bare[0],bare[1],bare[2],bare[3]);
        unsigned n=bare[2] | (unsigned)bare[3]<<8;
        if (bare[0]==0xa5 && n>=24 && n<=200) {
            s39_spi(h,0,h->data,n+3);
            for (unsigned i=0;i<n;i++) h->data[i]=h->data[i+2];
            h->length=n;
        }
    }
    if (s39_bringup(h)) printf("s3910: recovery bringup FAILED\n");
}

static void
s3910_thread(__unused void *arg, __unused wait_result_t wait)
{
    struct s39_hw *h=&hw;
    int rc=-1; unsigned reports=0, bad=0, errors=0, others=0, delta_frames=0;

    h->gpio=ml_io_map(S39_TLMM_PA,0x100000);
    h->rsc=ml_io_map(S39_RSC_PA,0x10000);
    h->db=ml_io_map_wcomb(S39_DB_PA,0x20000);
    h->spmi_core=ml_io_map(S39_SPMI_CORE_PA,0x4000);
    h->spmi_obs=ml_io_map(S39_SPMI_OBS_PA,0x10000);
    h->spmi_chn=ml_io_map(S39_SPMI_CHN_PA,0x400000);
    if (!h->gpio || !h->rsc || !h->db || !h->spmi_core || !h->spmi_obs || !h->spmi_chn) goto failed;

    rc=s39_power(h);
    printf("s3910: power rc=%d RSC=%#x ldob4=%#x slot=%u status=%#x\n",
        rc,h->rsc_id,h->regulator,h->slot,h->last_status);
    if (rc) goto failed;

    s39_bus_init(h); s39_reset(h);
    /* Android's detect_device, verbatim: write the 0x07 bus magic, read the
     * bare header, then the v1 continued read consumes the identify payload */
    {
        uint8_t bare[4];
        {
            uint8_t magic[1]={0x07};
            s39_spi(h,magic,0,1);
            s39_delay(100);
            s39_spi(h,0,bare,4);
        }
        printf("s3910: detect bare: %02x %02x %02x %02x\n",bare[0],bare[1],bare[2],bare[3]);
        unsigned n=bare[2] | (unsigned)bare[3]<<8;
        if (bare[0]!=0xa5 || n<24 || n>200) { rc=-52; goto failed; }
        s39_spi(h,0,h->data,n+3);          /* A5 03 payload... 5A */
        if (h->data[0]!=0xa5 || h->data[1]!=3 || h->data[n+2]!=0x5a) { rc=-53; goto failed; }
        for (unsigned i=0;i<n;i++) h->data[i]=h->data[i+2];
        h->length=n; rc=bare[1];
    }
    printf("s3910: boot msg rc=%d len=%u data=%02x %02x %c%c%c%c%c\n",
        rc,h->length,h->data[0],h->data[1],
        h->data[2],h->data[3],h->data[4],h->data[5],h->data[6]);
    if (rc!=0x10 || h->data[0]!=1 || h->data[1]!=1 ||
        memcmp(h->data+2,"S3910",5)) goto failed;

    if (s39_bringup(h)) { rc=-60; goto failed; }

    long last_report=0, recovering=0;
    for (unsigned tick=0;;tick++) {
        /* a finger we believe is down must produce reports at 125 Hz; half a
         * second of silence means the stream died mid-touch -> recover */
        if (active && !recovering && (long)tick-last_report > 500) {
            recovering=2000;               /* no re-entry for 2 s */
            s39_recover(h);
            last_report=tick;
        }
        if (recovering) recovering--;
        if (!s39_get(h,162) || tick%20==0) {
            rc=s39_read(h);
            while (1) {  /* drain back-to-back while reports keep coming */
                if (rc<0) {
                    if (++errors>=20) { errors=0; recovering=2000; s39_recover(h); last_report=tick; continue; }
                    bad++;
                } else {
                    errors=0;
                    if (rc==0x11) { /* REPORT_TOUCH */
                        struct s39_obj point[10]; unsigned mask=0;
                        if (s39_decode_frame(h->data,h->length,point,&mask)) {
                            bad++; release_all();
                        } else {
                            for (unsigned i=0;i<10;i++) {
                                if (mask&(1u<<i)) {
                                    unsigned nx=(uint32_t)point[i].x*65535/fw_max_x;
                                    unsigned ny=(uint32_t)point[i].y*65535/fw_max_y;
                                    hakios_touch_post(active&(1u<<i)?HAKIOS_TOUCH_MOVE:HAKIOS_TOUCH_DOWN,i,nx,ny);
                                    last[i]=point[i];
                                } else if (active&(1u<<i)) {
                                    hakios_touch_post(HAKIOS_TOUCH_UP,i,
                                        (uint32_t)last[i].x*65535/fw_max_x,
                                        (uint32_t)last[i].y*65535/fw_max_y);
                                }
                            }
                            active=mask; reports++; last_report=tick;
                            if (reports<=20) printf("s3910: report=%u active=%#x\n",reports,active);
                        }
                    } else if (rc==0x10) {
                        /* the firmware rebooted itself: bring the stream back */
                        release_all(); s39_recover(h); last_report=tick;
                    } else if (rc==0x12) { /* REPORT_DELTA: where are the peaks? */
                    delta_frames++;
                    int n=(int)h->length/2;
                    static unsigned shown;
                    int tv[5]={0}, ti[5]={0};
                    unsigned nz10=0;
                    for (int i=0;i<n;i++) {
                        int v=(int)(int16_t)s39_le16(h->data+2*i);
                        unsigned a=v<0?-v:v;
                        if (a>=10) nz10++;
                        for (int k=0;k<5;k++) if ((unsigned)a>(unsigned)tv[k]) {
                            for (int m=4;m>k;m--) { tv[m]=tv[m-1]; ti[m]=ti[m-1]; }
                            tv[k]=v; ti[k]=i; break;
                        }
                    }
                    if (delta_frames<=6) {   /* raw dump the first frames for offline decode */
                        printf("s3910: RAW f=%u len=%u:",delta_frames,h->length);
                        for (unsigned i=0;i<h->length && i<256;i++) printf("%02x",h->data[i]);
                        printf("\n");
                        if (h->length>256) {
                            printf("s3910: RAW f=%u tail:",delta_frames);
                            for (unsigned i=h->length-64;i<h->length;i++) printf("%02x",h->data[i]);
                            printf("\n");
                        }
                    }
                    if (shown++ % 200 == 0) {
                        int tv[5]={0}, ti[5]={0};
                        for (int i=0;i<n;i++) {
                            int v=(int)(int16_t)s39_le16(h->data+2*i);
                            unsigned a=v<0?-v:v;
                            for (int k=0;k<5;k++) if (a>(unsigned)(tv[k]<0?-tv[k]:tv[k])) {
                                for (int m=4;m>k;m--) { tv[m]=tv[m-1]; ti[m]=ti[m-1]; }
                                tv[k]=v; ti[k]=i; break;
                            }
                        }
                        printf("s3910: D f=%u top: %d@%d %d@%d %d@%d %d@%d %d@%d\n",
                            delta_frames,tv[0],ti[0],tv[1],ti[1],tv[2],ti[2],tv[3],ti[3],tv[4],ti[4]);
                    }
                                } else if (rc>=0x10) {
                        others++;
                    }
                }
                if (rc!=0x23) break;
                rc=s39_read(h);
            }
        }
        if (tick%2000==0)
            printf("s3910: t=%u.%03us reports=%u bad=%u active=%#x irq=%u\n",
                tick/1000,tick%1000,reports,bad,active,s39_get(h,162));
        s39_delay(1000);
    }
failed:
    printf("s3910: stopped rc=%d header=%02x %02x %02x %02x\n",rc,
        h->header[0],h->header[1],h->header[2],h->header[3]);
    thread_terminate(current_thread());
}

void
hakios_dodge_touch_init(void)
{
    uint32_t enabled=0; thread_t thread;
    if (!PE_parse_boot_argn("synaptics",&enabled,sizeof(enabled)) || enabled!=1) return;
    printf("synaptics: S3910 dodge preserve-mode\n");
    if (kernel_thread_start(s3910_thread,NULL,&thread)==KERN_SUCCESS)
        thread_deallocate(thread);
}
