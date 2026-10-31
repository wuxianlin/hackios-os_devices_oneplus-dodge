/*
 * OnePlus 13 (dodge) Synaptics S3910 touch: GPIO bit-bang SPI, TouchComm.
 * Hardware layer (power/transport) in hakios.dodge_touch_hw.h — proven.
 *
 * This version replicates the ANDROID/recovery minimal flow exactly:
 * power on -> hw reset -> read boot identify report -> GET_APP_INFO ->
 * GET_TOUCH_REPORT_CONFIG (preserve the firmware default!) -> poll reports.
 * No custom report config, no dynamic configs, no ENABLE_REPORT — the
 * firmware reports by default with its built-in config (that is how
 * Android recovery gets touch with no firmware file at all).
 *
 * Reports are decoded with a generic TouchComm config interpreter ported
 * from the OnePlus driver (synaptics_touchcom_func_touch.c).
 * Boot-arg: synaptics=1
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

#include "hakios.dodge_touch_hw.h"

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
