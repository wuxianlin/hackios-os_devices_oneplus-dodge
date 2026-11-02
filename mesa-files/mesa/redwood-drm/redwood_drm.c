/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"

#define REDWOOD_GPU_SERVICE     "HakiosGPU"
#define REDWOOD_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define REDWOOD_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

static io_connect_t connection;
static int connection_fd = -1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(REDWOOD_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static char *device_nodes[DRM_NODE_MAX] = { "", NULL, "/dev/null" };
static drmPlatformBusInfo device_bus = { "redwood-gpu" };
static drmDevice the_device = {
	.nodes = device_nodes,
	.available_nodes = 1 << DRM_NODE_RENDER,
	.bustype = DRM_BUS_PLATFORM,
	.businfo.platform = &device_bus,
};

/* The one device, once the connection to the kernel's driver is open (Turnip then opens the node itself). */
int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	int fd = drmOpenWithType("msm", NULL, 0);

	(void)flags;
	if (fd < 0) {
		return 0;
	}
	close(fd);
	if (devices != NULL && max_devices > 0) {
		devices[0] = &the_device;
	}
	return 1;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	(void)devices;
	(void)count;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	*device = &the_device;
	return 0;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	*device = NULL;
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a == b;
}

static int
error_from_return(kern_return_t result)
{
	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

static int
call_with_return(unsigned number, void *data, size_t size, kern_return_t *raw_result)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	if (raw_result != NULL) {
		*raw_result = result;
	}
	return error_from_return(result);
}

static int
call(unsigned number, void *data, size_t size)
{
	return call_with_return(number, data, size, NULL);
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

static int submit_with_syncobjs(struct drm_msm_gem_submit *submit, size_t size);

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	if (index == DRM_MSM_GEM_SUBMIT && size >= sizeof(struct drm_msm_gem_submit) &&
	    (((struct drm_msm_gem_submit *)data)->flags & (MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT))) {
		return -submit_with_syncobjs(data, size);
	}
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 7;
		version->name = strdup("msm");
		version->name_len = 3;
		version->date = strdup("");
		version->desc = strdup("Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

void *
redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = REDWOOD_GPU_DESCRIBE;
	error = call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)surface };
	kern_return_t raw_result = KERN_SUCCESS;
	int error = call_with_return(REDWOOD_GPU_SURFACE, &request, sizeof(request), &raw_result);

	(void)fd;
	if (error != 0) {
		static unsigned reports;
		if (__sync_fetch_and_add(&reports, 1) < 8) {
			fprintf(stderr, "redwood-drm: IOSurface %d import failed: IOReturn=%#x errno=%d\n",
			    surface, raw_result, error);
		}
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)
NONE(drmSyncobjTimelineSignal, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles, __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t handle_count)
NONE(drmSyncobjTimelineWait, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles, __attribute__((unused)) uint64_t *points, __attribute__((unused)) unsigned num_handles,
    __attribute__((unused)) int64_t timeout_nsec, __attribute__((unused)) unsigned flags, __attribute__((unused)) uint32_t *first_signaled)
NONE(drmSyncobjQuery, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles, __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t handle_count)
NONE(drmSyncobjQuery2, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles, __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t handle_count, __attribute__((unused)) uint32_t flags)

/*
 * Turnip: DRM syncobjs kept in this process. A syncobj is signaled, or carries the fence
 * of the submission that will signal it, or neither. The A830 runs one queue in order, so a
 * submission that waits for a syncobj only needs it to carry a fence (or be signaled) before
 * it goes to the kernel; the kernel itself sees no syncobjs.
 */
#define SYNCOBJS 4096
static struct { bool used, signaled; uint32_t fence; } syncobj[SYNCOBJS];
static pthread_mutex_t syncobj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t syncobj_change = PTHREAD_COND_INITIALIZER;

static bool
fence_done(uint32_t fence)
{
	struct drm_msm_wait_fence req = { .fence = fence };
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	req.timeout.tv_sec = now.tv_sec;
	req.timeout.tv_nsec = now.tv_nsec;
	return call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &req, sizeof(req)) == 0;
}

static bool
syncobj_valid(uint32_t handle)
{
	return handle != 0 && handle < SYNCOBJS && syncobj[handle].used;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 1; i < SYNCOBJS; i++) {
		if (!syncobj[i].used) {
			syncobj[i].used = true;
			syncobj[i].signaled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
			syncobj[i].fence = 0;
			*handle = i;
			pthread_mutex_unlock(&syncobj_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	errno = ENOMEM;
	return -ENOMEM;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	if (syncobj_valid(handle)) {
		syncobj[handle].used = false;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

static int
syncobj_set(const uint32_t *handles, uint32_t count, bool signaled)
{
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		if (!syncobj_valid(handles[i])) {
			pthread_mutex_unlock(&syncobj_lock);
			errno = EINVAL;
			return -EINVAL;
		}
		syncobj[handles[i]].signaled = signaled;
		syncobj[handles[i]].fence = 0;
	}
	pthread_cond_broadcast(&syncobj_change);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return syncobj_set(handles, count, false);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return syncobj_set(handles, count, true);
}

int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)flags;
	if (dst_point != 0 || src_point != 0) {
		errno = EINVAL;
		return -EINVAL;
	}
	pthread_mutex_lock(&syncobj_lock);
	if (!syncobj_valid(dst) || !syncobj_valid(src)) {
		pthread_mutex_unlock(&syncobj_lock);
		errno = EINVAL;
		return -EINVAL;
	}
	syncobj[dst].signaled = syncobj[src].signaled;
	syncobj[dst].fence = syncobj[src].fence;
	pthread_cond_broadcast(&syncobj_change);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

/* With syncobj_lock held: 1 done, 0 not yet, -1 can never be (no fence and not to wait for one). */
static int
syncobj_state(uint32_t handle, unsigned flags)
{
	if (syncobj[handle].signaled) {
		return 1;
	}
	if (syncobj[handle].fence != 0) {
		if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) {
			return 1;
		}
		if (fence_done(syncobj[handle].fence)) {
			syncobj[handle].signaled = true;
			return 1;
		}
		return 0;
	}
	return (flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) ? 0 : -1;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (;;) {
		unsigned done = 0;
		int first = -1;

		for (unsigned i = 0; i < count; i++) {
			int state;

			if (!syncobj_valid(handles[i])) {
				pthread_mutex_unlock(&syncobj_lock);
				errno = EINVAL;
				return -EINVAL;
			}
			state = syncobj_state(handles[i], flags);
			if (state < 0) {
				pthread_mutex_unlock(&syncobj_lock);
				errno = EINVAL;
				return -EINVAL;
			}
			if (state > 0) {
				done++;
				if (first < 0) {
					first = (int)i;
				}
			}
		}
		if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? done == count : done > 0) {
			if (first_signaled != NULL && first >= 0) {
				*first_signaled = (uint32_t)first;
			}
			pthread_mutex_unlock(&syncobj_lock);
			return 0;
		}

		struct timespec now, until;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((int64_t)now.tv_sec * 1000000000 + now.tv_nsec >= timeout_nsec) {
			pthread_mutex_unlock(&syncobj_lock);
			errno = ETIME;
			return -ETIME;
		}
		/* a fence is looked at again every 100 us; a syncobj without one waits to be given one */
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 100000;
		if (until.tv_nsec >= 1000000000) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&syncobj_change, &syncobj_lock, &until);
	}
}

/* DRM_MSM_GEM_SUBMIT with syncobjs: waits for the inputs to carry fences, submits without them, gives the outputs the fence. */
static int
submit_with_syncobjs(struct drm_msm_gem_submit *submit, size_t size)
{
	uint32_t flags = submit->flags, stride = submit->syncobj_stride;
	const uint8_t *in = (const uint8_t *)(uintptr_t)submit->in_syncobjs;
	const uint8_t *out = (const uint8_t *)(uintptr_t)submit->out_syncobjs;
	uint32_t nin = submit->nr_in_syncobjs, nout = submit->nr_out_syncobjs;
	int error;

	if ((flags & MSM_SUBMIT_SYNCOBJ_IN) && nin != 0) {
		uint32_t handles[64];

		if (nin > 64 || stride < sizeof(struct drm_msm_syncobj)) {
			return EINVAL;
		}
		for (uint32_t i = 0; i < nin; i++) {
			handles[i] = ((const struct drm_msm_syncobj *)(in + (size_t)i * stride))->handle;
		}
		if (drmSyncobjWait(-1, handles, nin, INT64_MAX,
		    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, NULL) != 0) {
			return errno;
		}
	}
	submit->flags = flags & ~(MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT);
	submit->in_syncobjs = submit->out_syncobjs = 0;
	submit->nr_in_syncobjs = submit->nr_out_syncobjs = submit->syncobj_stride = 0;
	error = call(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, submit, size);
	if (error == 0 && (flags & MSM_SUBMIT_SYNCOBJ_OUT)) {
		pthread_mutex_lock(&syncobj_lock);
		for (uint32_t i = 0; i < nout; i++) {
			uint32_t handle = ((const struct drm_msm_syncobj *)(out + (size_t)i * stride))->handle;

			if (syncobj_valid(handle)) {
				syncobj[handle].signaled = false;
				syncobj[handle].fence = submit->fence;
			}
		}
		pthread_cond_broadcast(&syncobj_change);
		pthread_mutex_unlock(&syncobj_lock);
	}
	return error;
}
