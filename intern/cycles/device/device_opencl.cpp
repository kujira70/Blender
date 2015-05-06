/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef WITH_OPENCL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clew.h"

#include "device.h"
#include "device_intern.h"

#include "buffers.h"

#include "util_foreach.h"
#include "util_logging.h"
#include "util_map.h"
#include "util_math.h"
#include "util_md5.h"
#include "util_opengl.h"
#include "util_path.h"
#include "util_time.h"

CCL_NAMESPACE_BEGIN

#define CL_MEM_PTR(p) ((cl_mem)(uintptr_t)(p))

/* Macro declarations used with split kernel */

/* Macro to enable/disable work-stealing */
#define __WORK_STEALING__

#define SPLIT_KERNEL_LOCAL_SIZE_X 64
#define SPLIT_KERNEL_LOCAL_SIZE_Y 1

/* This value may be tuned according to the scene we are rendering */
/* modifying PATH_ITER_INC_FACTOR value proportional to number of expected ray-bounces will improve performance */
#define PATH_ITER_INC_FACTOR 8

/*
 * When allocate global memory in chunks. We may not be able to
 * allocate exactly "CL_DEVICE_MAX_MEM_ALLOC_SIZE" bytes in chunks;
 * Since some bytes may be needed for aligning chunks of memory;
 * This is the amount of memory that we dedicate for that purpose.
 */
#define DATA_ALLOCATION_MEM_FACTOR 5000000 //5MB

static cl_device_type opencl_device_type()
{
	char *device = getenv("CYCLES_OPENCL_TEST");

	if(device) {
		if(strcmp(device, "ALL") == 0)
			return CL_DEVICE_TYPE_ALL;
		else if(strcmp(device, "DEFAULT") == 0)
			return CL_DEVICE_TYPE_DEFAULT;
		else if(strcmp(device, "CPU") == 0)
			return CL_DEVICE_TYPE_CPU;
		else if(strcmp(device, "GPU") == 0)
			return CL_DEVICE_TYPE_GPU;
		else if(strcmp(device, "ACCELERATOR") == 0)
			return CL_DEVICE_TYPE_ACCELERATOR;
	}

	return CL_DEVICE_TYPE_ALL;
}

static bool opencl_kernel_use_debug()
{
	return (getenv("CYCLES_OPENCL_DEBUG") != NULL);
}

static bool opencl_kernel_use_advanced_shading(const string& platform)
{
	/* keep this in sync with kernel_types.h! */
	if(platform == "NVIDIA CUDA")
		return true;
	else if(platform == "Apple")
		return false;
	else if(platform == "AMD Accelerated Parallel Processing")
		return false;
	else if(platform == "Intel(R) OpenCL")
		return true;

	return false;
}

static string opencl_kernel_build_options(const string& platform, const string *debug_src = NULL)
{
	string build_options = " -cl-fast-relaxed-math ";

	if(platform == "NVIDIA CUDA")
		build_options += "-D__KERNEL_OPENCL_NVIDIA__ -cl-nv-maxrregcount=32 -cl-nv-verbose ";

	else if(platform == "Apple")
		build_options += "-D__KERNEL_OPENCL_APPLE__ ";

	else if(platform == "AMD Accelerated Parallel Processing")
		build_options += "-D__KERNEL_OPENCL_AMD__ ";

	else if(platform == "Intel(R) OpenCL") {
		build_options += "-D__KERNEL_OPENCL_INTEL_CPU__ ";

		/* options for gdb source level kernel debugging. this segfaults on linux currently */
		if(opencl_kernel_use_debug() && debug_src)
			build_options += "-g -s \"" + *debug_src + "\" ";
	}

	if(opencl_kernel_use_debug())
		build_options += "-D__KERNEL_OPENCL_DEBUG__ ";

#ifdef WITH_CYCLES_DEBUG
	build_options += "-D__KERNEL_DEBUG__ ";
#endif

	return build_options;
}

/* thread safe cache for contexts and programs */
class OpenCLCache
{
	struct Slot
	{
		thread_mutex *mutex;
		cl_context context;
		/* cl_program for shader, bake, film_convert kernels (used in OpenCLDeviceBase) */
		cl_program ocl_dev_base_program;
		/* cl_program for megakernel (used in OpenCLDeviceMegaKernel) */
		cl_program ocl_dev_megakernel_program;

		Slot() : mutex(NULL), context(NULL), ocl_dev_base_program(NULL), ocl_dev_megakernel_program(NULL) {}

		Slot(const Slot &rhs)
			: mutex(rhs.mutex)
			, context(rhs.context)
			, ocl_dev_base_program(rhs.ocl_dev_base_program)
			, ocl_dev_megakernel_program(rhs.ocl_dev_megakernel_program)
		{
			/* copy can only happen in map insert, assert that */
			assert(mutex == NULL);
		}

		~Slot()
		{
			delete mutex;
			mutex = NULL;
		}
	};

	/* key is combination of platform ID and device ID */
	typedef pair<cl_platform_id, cl_device_id> PlatformDevicePair;

	/* map of Slot objects */
	typedef map<PlatformDevicePair, Slot> CacheMap;
	CacheMap cache;

	thread_mutex cache_lock;

	/* lazy instantiate */
	static OpenCLCache &global_instance()
	{
		static OpenCLCache instance;
		return instance;
	}

	OpenCLCache()
	{
	}

	~OpenCLCache()
	{
		/* Intel OpenCL bug raises SIGABRT due to pure virtual call
		 * so this is disabled. It's not necessary to free objects
		 * at process exit anyway.
		 * http://software.intel.com/en-us/forums/topic/370083#comments */

		//flush();
	}

	/* lookup something in the cache. If this returns NULL, slot_locker
	 * will be holding a lock for the cache. slot_locker should refer to a
	 * default constructed thread_scoped_lock */
	template<typename T>
	static T get_something(cl_platform_id platform, cl_device_id device,
		T Slot::*member, thread_scoped_lock &slot_locker)
	{
		assert(platform != NULL);

		OpenCLCache &self = global_instance();

		thread_scoped_lock cache_lock(self.cache_lock);

		pair<CacheMap::iterator,bool> ins = self.cache.insert(
			CacheMap::value_type(PlatformDevicePair(platform, device), Slot()));

		Slot &slot = ins.first->second;

		/* create slot lock only while holding cache lock */
		if(!slot.mutex)
			slot.mutex = new thread_mutex;

		/* need to unlock cache before locking slot, to allow store to complete */
		cache_lock.unlock();

		/* lock the slot */
		slot_locker = thread_scoped_lock(*slot.mutex);

		/* If the thing isn't cached */
		if(slot.*member == NULL) {
			/* return with the caller's lock holder holding the slot lock */
			return NULL;
		}

		/* the item was already cached, release the slot lock */
		slot_locker.unlock();

		return slot.*member;
	}

	/* store something in the cache. you MUST have tried to get the item before storing to it */
	template<typename T>
	static void store_something(cl_platform_id platform, cl_device_id device, T thing,
		T Slot::*member, thread_scoped_lock &slot_locker)
	{
		assert(platform != NULL);
		assert(device != NULL);
		assert(thing != NULL);

		OpenCLCache &self = global_instance();

		thread_scoped_lock cache_lock(self.cache_lock);
		CacheMap::iterator i = self.cache.find(PlatformDevicePair(platform, device));
		cache_lock.unlock();

		Slot &slot = i->second;

		/* sanity check */
		assert(i != self.cache.end());
		assert(slot.*member == NULL);

		slot.*member = thing;

		/* unlock the slot */
		slot_locker.unlock();
	}

public:

	enum ProgramName {
		OCL_DEV_BASE_PROGRAM,
		OCL_DEV_MEGAKERNEL_PROGRAM,
	};

	/* see get_something comment */
	static cl_context get_context(cl_platform_id platform, cl_device_id device,
		thread_scoped_lock &slot_locker)
	{
		cl_context context = get_something<cl_context>(platform, device, &Slot::context, slot_locker);

		if(!context)
			return NULL;

		/* caller is going to release it when done with it, so retain it */
		cl_int ciErr = clRetainContext(context);
		assert(ciErr == CL_SUCCESS);
		(void)ciErr;

		return context;
	}

	/* see get_something comment */
	static cl_program get_program(cl_platform_id platform, cl_device_id device, ProgramName program_name,
		thread_scoped_lock &slot_locker)
	{
		cl_program program = NULL;

		if(program_name == OCL_DEV_BASE_PROGRAM) {
			/* Get program related to OpenCLDeviceBase */
			program = get_something<cl_program>(platform, device, &Slot::ocl_dev_base_program, slot_locker);
		}
		else if(program_name == OCL_DEV_MEGAKERNEL_PROGRAM) {
			/* Get program related to megakernel */
			program = get_something<cl_program>(platform, device, &Slot::ocl_dev_megakernel_program, slot_locker);
		} else {
			fprintf(stderr, "Invalid program name in OpenCLCache \n");
		}

		if(!program)
			return NULL;

		/* caller is going to release it when done with it, so retain it */
		cl_int ciErr = clRetainProgram(program);
		assert(ciErr == CL_SUCCESS);
		(void)ciErr;

		return program;
	}

	/* see store_something comment */
	static void store_context(cl_platform_id platform, cl_device_id device, cl_context context,
		thread_scoped_lock &slot_locker)
	{
		store_something<cl_context>(platform, device, context, &Slot::context, slot_locker);

		/* increment reference count in OpenCL.
		 * The caller is going to release the object when done with it. */
		cl_int ciErr = clRetainContext(context);
		assert(ciErr == CL_SUCCESS);
		(void)ciErr;
	}

	/* see store_something comment */
	static void store_program(cl_platform_id platform, cl_device_id device, cl_program program, ProgramName program_name,
		thread_scoped_lock &slot_locker)
	{
		if(program_name == OCL_DEV_BASE_PROGRAM) {
			store_something<cl_program>(platform, device, program, &Slot::ocl_dev_base_program, slot_locker);
		}
		else if(program_name == OCL_DEV_MEGAKERNEL_PROGRAM) {
			store_something<cl_program>(platform, device, program, &Slot::ocl_dev_megakernel_program, slot_locker);
		} else {
			fprintf(stderr, "Invalid program name in OpenCLCache \n");
			return;
		}

		/* increment reference count in OpenCL.
		 * The caller is going to release the object when done with it. */
		cl_int ciErr = clRetainProgram(program);
		assert(ciErr == CL_SUCCESS);
		(void)ciErr;
	}

	/* discard all cached contexts and programs
	 * the parameter is a temporary workaround. See OpenCLCache::~OpenCLCache */
	static void flush()
	{
		OpenCLCache &self = global_instance();
		thread_scoped_lock cache_lock(self.cache_lock);

		foreach(CacheMap::value_type &item, self.cache) {
			if(item.second.ocl_dev_base_program != NULL)
				clReleaseProgram(item.second.ocl_dev_base_program);
			if(item.second.ocl_dev_megakernel_program != NULL)
				clReleaseProgram(item.second.ocl_dev_megakernel_program);
			if(item.second.context != NULL)
				clReleaseContext(item.second.context);
		}

		self.cache.clear();
	}
};

class OpenCLDeviceBase : public Device
{
public:
	cl_context cxContext;
	cl_command_queue cqCommandQueue;
	cl_platform_id cpPlatform;
	cl_device_id cdDevice;
	cl_int ciErr;

	cl_kernel ckShaderKernel;
	cl_kernel ckBakeKernel;
	cl_kernel ckFilmConvertByteKernel;
	cl_kernel ckFilmConvertHalfFloatKernel;
	cl_program cpProgram;

	typedef map<string, device_vector<uchar>*> ConstMemMap;
	typedef map<string, device_ptr> MemMap;

	ConstMemMap const_mem_map;
	MemMap mem_map;
	device_ptr null_mem;

	bool device_initialized;
	string platform_name;

	bool opencl_error(cl_int err)
	{
		if(err != CL_SUCCESS) {
			string message = string_printf("OpenCL error (%d): %s", err, clewErrorString(err));
			if(error_msg == "")
				error_msg = message;
			fprintf(stderr, "%s\n", message.c_str());
			return true;
		}

		return false;
	}

	void opencl_error(const string& message)
	{
		if(error_msg == "")
			error_msg = message;
		fprintf(stderr, "%s\n", message.c_str());
	}

#define opencl_assert(stmt) \
	{ \
		cl_int err = stmt; \
		if(err != CL_SUCCESS) { \
			string message = string_printf("OpenCL error: %s in %s", clewErrorString(err), #stmt); \
			if(error_msg == "") \
				error_msg = message; \
			fprintf(stderr, "%s\n", message.c_str()); \
		} \
	} (void)0

	void opencl_assert_err(cl_int err, const char* where)
	{
		if(err != CL_SUCCESS) {
			string message = string_printf("OpenCL error (%d): %s in %s", err, clewErrorString(err), where);
			if(error_msg == "")
				error_msg = message;
			fprintf(stderr, "%s\n", message.c_str());
#ifndef NDEBUG
			abort();
#endif
		}
	}

	OpenCLDeviceBase(DeviceInfo& info, Stats &stats, bool background_)
		: Device(info, stats, background_)
	{
		cpPlatform = NULL;
		cdDevice = NULL;
		cxContext = NULL;
		cqCommandQueue = NULL;
		null_mem = 0;
		device_initialized = false;

		ckShaderKernel = NULL;
		ckBakeKernel = NULL;
		ckFilmConvertByteKernel = NULL;
		ckFilmConvertHalfFloatKernel = NULL;
		cpProgram = NULL;

		/* setup platform */
		cl_uint num_platforms;

		ciErr = clGetPlatformIDs(0, NULL, &num_platforms);
		if(opencl_error(ciErr))
			return;

		if(num_platforms == 0) {
			opencl_error("OpenCL: no platforms found.");
			return;
		}

		vector<cl_platform_id> platforms(num_platforms, NULL);

		ciErr = clGetPlatformIDs(num_platforms, &platforms[0], NULL);
		if(opencl_error(ciErr)) {
			fprintf(stderr, "clGetPlatformIDs failed \n");
			return;
		}

		int num_base = 0;
		int total_devices = 0;

		for(int platform = 0; platform < num_platforms; platform++) {
			cl_uint num_devices;

			if(opencl_error(clGetDeviceIDs(platforms[platform], opencl_device_type(), 0, NULL, &num_devices)))
				return;

			total_devices += num_devices;

			if(info.num - num_base >= num_devices) {
				/* num doesn't refer to a device in this platform */
				num_base += num_devices;
				continue;
			}

			/* device is in this platform */
			cpPlatform = platforms[platform];

			/* get devices */
			vector<cl_device_id> device_ids(num_devices, NULL);

			if(opencl_error(clGetDeviceIDs(cpPlatform, opencl_device_type(), num_devices, &device_ids[0], NULL))) {
				fprintf(stderr, "clGetDeviceIDs failed \n");
				return;
			}

			cdDevice = device_ids[info.num - num_base];

			char name[256];
			clGetPlatformInfo(cpPlatform, CL_PLATFORM_NAME, sizeof(name), &name, NULL);
			platform_name = name;

			break;
		}

		if(total_devices == 0) {
			opencl_error("OpenCL: no devices found.");
			return;
		}
		else if(!cdDevice) {
			opencl_error("OpenCL: specified device not found.");
			return;
		}

		{
			/* try to use cached context */
			thread_scoped_lock cache_locker;
			cxContext = OpenCLCache::get_context(cpPlatform, cdDevice, cache_locker);

			if(cxContext == NULL) {
				/* create context properties array to specify platform */
				const cl_context_properties context_props[] = {
					CL_CONTEXT_PLATFORM, (cl_context_properties)cpPlatform,
					0, 0
				};

				/* create context */
				cxContext = clCreateContext(context_props, 1, &cdDevice,
					context_notify_callback, cdDevice, &ciErr);

				if(opencl_error(ciErr)) {
					opencl_error("OpenCL: clCreateContext failed");
					return;
				}

				/* cache it */
				OpenCLCache::store_context(cpPlatform, cdDevice, cxContext, cache_locker);
			}
		}

		cqCommandQueue = clCreateCommandQueue(cxContext, cdDevice, 0, &ciErr);
		if(opencl_error(ciErr))
			return;

		null_mem = (device_ptr)clCreateBuffer(cxContext, CL_MEM_READ_ONLY, 1, NULL, &ciErr);
		if(opencl_error(ciErr))
			return;

		fprintf(stderr, "Device init success\n");
		device_initialized = true;
	}

	static void CL_CALLBACK context_notify_callback(const char *err_info,
		const void * /*private_info*/, size_t /*cb*/, void *user_data)
	{
		char name[256];
		clGetDeviceInfo((cl_device_id)user_data, CL_DEVICE_NAME, sizeof(name), &name, NULL);

		fprintf(stderr, "OpenCL error (%s): %s\n", name, err_info);
	}

	bool opencl_version_check()
	{
		char version[256];

		int major, minor, req_major = 1, req_minor = 1;

		clGetPlatformInfo(cpPlatform, CL_PLATFORM_VERSION, sizeof(version), &version, NULL);

		if(sscanf(version, "OpenCL %d.%d", &major, &minor) < 2) {
			opencl_error(string_printf("OpenCL: failed to parse platform version string (%s).", version));
			return false;
		}

		if(!((major == req_major && minor >= req_minor) || (major > req_major))) {
			opencl_error(string_printf("OpenCL: platform version 1.1 or later required, found %d.%d", major, minor));
			return false;
		}

		clGetDeviceInfo(cdDevice, CL_DEVICE_OPENCL_C_VERSION, sizeof(version), &version, NULL);

		if(sscanf(version, "OpenCL C %d.%d", &major, &minor) < 2) {
			opencl_error(string_printf("OpenCL: failed to parse OpenCL C version string (%s).", version));
			return false;
		}

		if(!((major == req_major && minor >= req_minor) || (major > req_major))) {
			opencl_error(string_printf("OpenCL: C version 1.1 or later required, found %d.%d", major, minor));
			return false;
		}

		return true;
	}

	string device_md5_hash(string kernel_custom_build_option)
	{
		MD5Hash md5;
		char version[256], driver[256], name[256], vendor[256];

		clGetPlatformInfo(cpPlatform, CL_PLATFORM_VENDOR, sizeof(vendor), &vendor, NULL);
		clGetDeviceInfo(cdDevice, CL_DEVICE_VERSION, sizeof(version), &version, NULL);
		clGetDeviceInfo(cdDevice, CL_DEVICE_NAME, sizeof(name), &name, NULL);
		clGetDeviceInfo(cdDevice, CL_DRIVER_VERSION, sizeof(driver), &driver, NULL);

		md5.append((uint8_t*)vendor, strlen(vendor));
		md5.append((uint8_t*)version, strlen(version));
		md5.append((uint8_t*)name, strlen(name));
		md5.append((uint8_t*)driver, strlen(driver));

		string options;

		options = opencl_kernel_build_options(platform_name) + kernel_custom_build_option;
		md5.append((uint8_t*)options.c_str(), options.size());

		return md5.get_hex();
	}

	~OpenCLDeviceBase()
	{

		if(null_mem)
			clReleaseMemObject(CL_MEM_PTR(null_mem));

		ConstMemMap::iterator mt;
		for(mt = const_mem_map.begin(); mt != const_mem_map.end(); mt++) {
			mem_free(*(mt->second));
			delete mt->second;
		}

		if(ckBakeKernel)
			clReleaseKernel(ckBakeKernel);

		if(ckShaderKernel)
			clReleaseKernel(ckShaderKernel);

		if(ckFilmConvertByteKernel)
			clReleaseKernel(ckFilmConvertByteKernel);

		if(ckFilmConvertHalfFloatKernel)
			clReleaseKernel(ckFilmConvertHalfFloatKernel);

		if(cpProgram)
			clReleaseProgram(cpProgram);

		if(cqCommandQueue)
			clReleaseCommandQueue(cqCommandQueue);
		if(cxContext)
			clReleaseContext(cxContext);
	}

	void mem_alloc(device_memory& mem, MemoryType type)
	{
		size_t size = mem.memory_size();

		cl_mem_flags mem_flag;
		void *mem_ptr = NULL;

		if(type == MEM_READ_ONLY)
			mem_flag = CL_MEM_READ_ONLY;
		else if(type == MEM_WRITE_ONLY)
			mem_flag = CL_MEM_WRITE_ONLY;
		else
			mem_flag = CL_MEM_READ_WRITE;

		mem.device_pointer = (device_ptr)clCreateBuffer(cxContext, mem_flag, size, mem_ptr, &ciErr);

		opencl_assert_err(ciErr, "clCreateBuffer");

		stats.mem_alloc(size);
		mem.device_size = size;
	}

	/* overloading function for creating split kernel device buffers */
	cl_mem mem_alloc(size_t bufsize, cl_mem_flags mem_flag = CL_MEM_READ_WRITE) {
		cl_mem ptr;
		ptr = clCreateBuffer(cxContext, mem_flag, bufsize, NULL, &ciErr);
		if (ciErr != CL_SUCCESS) {
			fprintf(stderr, "(%d) %s in clCreateBuffer\n", ciErr, clewErrorString(ciErr));
			assert(0);
		}
		return ptr;
	}

	void mem_copy_to(device_memory& mem)
	{
		/* this is blocking */
		size_t size = mem.memory_size();
		opencl_assert(clEnqueueWriteBuffer(cqCommandQueue, CL_MEM_PTR(mem.device_pointer), CL_TRUE, 0, size, (void*)mem.data_pointer, 0, NULL, NULL));
	}

	void mem_copy_from(device_memory& mem, int y, int w, int h, int elem)
	{
		size_t offset = elem*y*w;
		size_t size = elem*w*h;

		opencl_assert(clEnqueueReadBuffer(cqCommandQueue, CL_MEM_PTR(mem.device_pointer), CL_TRUE, offset, size, (uchar*)mem.data_pointer + offset, 0, NULL, NULL));
	}

	void mem_zero(device_memory& mem)
	{
		if(mem.device_pointer) {
			memset((void*)mem.data_pointer, 0, mem.memory_size());
			mem_copy_to(mem);
		}
	}

	void mem_free(device_memory& mem)
	{
		if(mem.device_pointer) {
			opencl_assert(clReleaseMemObject(CL_MEM_PTR(mem.device_pointer)));
			mem.device_pointer = 0;

			stats.mem_free(mem.device_size);
			mem.device_size = 0;
		}
	}

	void const_copy_to(const char *name, void *host, size_t size)
	{
		ConstMemMap::iterator i = const_mem_map.find(name);

		if(i == const_mem_map.end()) {
			device_vector<uchar> *data = new device_vector<uchar>();
			data->copy((uchar*)host, size);

			mem_alloc(*data, MEM_READ_ONLY);
			i = const_mem_map.insert(ConstMemMap::value_type(name, data)).first;
		}
		else {
			device_vector<uchar> *data = i->second;
			data->copy((uchar*)host, size);
		}

		mem_copy_to(*i->second);
	}

	void tex_alloc(const char *name,
		device_memory& mem,
		InterpolationType /*interpolation*/,
		bool /*periodic*/)
	{
		VLOG(1) << "Texture allocate: " << name << ", " << mem.memory_size() << " bytes.";
		mem_alloc(mem, MEM_READ_ONLY);
		mem_copy_to(mem);
		assert(mem_map.find(name) == mem_map.end());
		mem_map.insert(MemMap::value_type(name, mem.device_pointer));
	}

	void tex_free(device_memory& mem)
	{
		if(mem.device_pointer) {
			foreach(const MemMap::value_type& value, mem_map) {
				if(value.second == mem.device_pointer) {
					mem_map.erase(value.first);
					break;
				}
			}

			mem_free(mem);
		}
	}

	bool load_binary(const string& /*kernel_path*/,
	                 const string& clbin,
	                 string custom_kernel_build_options,
	                 cl_program *program,
	                 const string *debug_src = NULL)
	{
		/* read binary into memory */
		vector<uint8_t> binary;

		if(!path_read_binary(clbin, binary)) {
			opencl_error(string_printf("OpenCL failed to read cached binary %s.", clbin.c_str()));
			return false;
		}

		/* create program */
		cl_int status;
		size_t size = binary.size();
		const uint8_t *bytes = &binary[0];

		*program = clCreateProgramWithBinary(cxContext, 1, &cdDevice,
			&size, &bytes, &status, &ciErr);

		if(opencl_error(status) || opencl_error(ciErr)) {
			opencl_error(string_printf("OpenCL failed create program from cached binary %s.", clbin.c_str()));
			return false;
		}

		if(!build_kernel(program, custom_kernel_build_options, debug_src))
			return false;

		return true;
	}

	bool save_binary(cl_program *program, const string& clbin) {
		size_t size = 0;
		clGetProgramInfo(*program, CL_PROGRAM_BINARY_SIZES, sizeof(size_t), &size, NULL);

		if(!size)
			return false;

		vector<uint8_t> binary(size);
		uint8_t *bytes = &binary[0];

		clGetProgramInfo(*program, CL_PROGRAM_BINARIES, sizeof(uint8_t*), &bytes, NULL);

		if(!path_write_binary(clbin, binary)) {
			opencl_error(string_printf("OpenCL failed to write cached binary %s.", clbin.c_str()));
			return false;
		}

		return true;
	}

	bool build_kernel(cl_program *kernel_program,
		string custom_kernel_build_options,
		const string *debug_src = NULL)
	{
		string build_options;
		build_options = opencl_kernel_build_options(platform_name, debug_src) + custom_kernel_build_options;

		ciErr = clBuildProgram(*kernel_program, 0, NULL, build_options.c_str(), NULL, NULL);

		/* show warnings even if build is successful */
		size_t ret_val_size = 0;

		clGetProgramBuildInfo(*kernel_program, cdDevice, CL_PROGRAM_BUILD_LOG, 0, NULL, &ret_val_size);

		if(ret_val_size > 1) {
			vector<char> build_log(ret_val_size + 1);
			clGetProgramBuildInfo(*kernel_program, cdDevice, CL_PROGRAM_BUILD_LOG, ret_val_size, &build_log[0], NULL);

			build_log[ret_val_size] = '\0';
			fprintf(stderr, "OpenCL kernel build output:\n");
			fprintf(stderr, "%s\n", &build_log[0]);
		}

		if(ciErr != CL_SUCCESS) {
			opencl_error("OpenCL build failed: errors in console");
			return false;
		}

		return true;
	}


	bool compile_kernel(const string& kernel_path,
		string source,
		string custom_kernel_build_options,
		cl_program *kernel_program,
		const string *debug_src = NULL)
	{
		/* we compile kernels consisting of many files. unfortunately opencl
		* kernel caches do not seem to recognize changes in included files.
		* so we force recompile on changes by adding the md5 hash of all files */
		source = path_source_replace_includes(source, kernel_path);

		if(debug_src)
			path_write_text(*debug_src, source);

		size_t source_len = source.size();
		const char *source_str = source.c_str();

		*kernel_program = clCreateProgramWithSource(cxContext, 1, &source_str, &source_len, &ciErr);

		if(opencl_error(ciErr))
			return false;

		double starttime = time_dt();
		printf("Compiling OpenCL kernel ...\n");

		if(!build_kernel(kernel_program, custom_kernel_build_options, debug_src))
			return false;

		printf("Kernel compilation finished in %.2lfs.\n", time_dt() - starttime);

		return true;
	}


	bool load_kernels(bool /*experimental*/)
	{
		/* verify if device was initialized */
		if(!device_initialized) {
			fprintf(stderr, "OpenCL: failed to initialize device.\n");
			return false;
		}

		/* try to use cached kernel */
		thread_scoped_lock cache_locker;
		cpProgram = OpenCLCache::get_program(cpPlatform, cdDevice, OpenCLCache::OCL_DEV_BASE_PROGRAM, cache_locker);

		if(!cpProgram) {
			/* verify we have right opencl version */
			if(!opencl_version_check())
				return false;

			/* md5 hash to detect changes */
			string kernel_path = path_get("kernel");
			string kernel_md5 = path_files_md5_hash(kernel_path);
			string custom_kernel_build_options = "";
			string device_md5 = device_md5_hash(custom_kernel_build_options);

			/* path to cached binary */
			string clbin = string_printf("cycles_kernel_%s_%s.clbin", device_md5.c_str(), kernel_md5.c_str());
			clbin = path_user_get(path_join("cache", clbin));

			/* path to preprocessed source for debugging */
			string clsrc, *debug_src = NULL;

			if(opencl_kernel_use_debug()) {
				clsrc = string_printf("cycles_kernel_%s_%s.cl", device_md5.c_str(), kernel_md5.c_str());
				clsrc = path_user_get(path_join("cache", clsrc));
				debug_src = &clsrc;
			}

			/* if exists already, try use it */
			if(path_exists(clbin) && load_binary(kernel_path, clbin, custom_kernel_build_options, &cpProgram)) {
				/* kernel loaded from binary */
			}
			else {

				string init_kernel_source = "#include \"kernel.cl\" // " + kernel_md5 + "\n";

				/* if does not exist or loading binary failed, compile kernel */
				if (!compile_kernel(kernel_path, init_kernel_source, custom_kernel_build_options, &cpProgram, debug_src))
					return false;

				/* save binary for reuse */
				if(!save_binary(&cpProgram, clbin))
					return false;
			}

			/* cache the program */
			OpenCLCache::store_program(cpPlatform, cdDevice, cpProgram, OpenCLCache::OCL_DEV_BASE_PROGRAM, cache_locker);
		}

		/* find kernels */
		ckShaderKernel = clCreateKernel(cpProgram, "kernel_ocl_shader", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckBakeKernel = clCreateKernel(cpProgram, "kernel_ocl_bake", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckFilmConvertByteKernel = clCreateKernel(cpProgram, "kernel_ocl_convert_to_byte", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckFilmConvertHalfFloatKernel = clCreateKernel(cpProgram, "kernel_ocl_convert_to_half_float", &ciErr);
		if(opencl_error(ciErr))
			return false;

		return true;
	}

	size_t global_size_round_up(int group_size, int global_size)
	{
		int r = global_size % group_size;
		return global_size + ((r == 0) ? 0 : group_size - r);
	}

	void enqueue_kernel(cl_kernel kernel, size_t w, size_t h)
	{
		size_t workgroup_size, max_work_items[3];

		clGetKernelWorkGroupInfo(kernel, cdDevice,
			CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), &workgroup_size, NULL);
		clGetDeviceInfo(cdDevice,
			CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(size_t)* 3, max_work_items, NULL);

		/* try to divide evenly over 2 dimensions */
		size_t sqrt_workgroup_size = max((size_t)sqrt((double)workgroup_size), 1);
		size_t local_size[2] = { sqrt_workgroup_size, sqrt_workgroup_size };

		/* some implementations have max size 1 on 2nd dimension */
		if(local_size[1] > max_work_items[1]) {
			local_size[0] = workgroup_size / max_work_items[1];
			local_size[1] = max_work_items[1];
		}

		size_t global_size[2] = { global_size_round_up(local_size[0], w), global_size_round_up(local_size[1], h) };

		/* run kernel */
		opencl_assert(clEnqueueNDRangeKernel(cqCommandQueue, kernel, 2, NULL, global_size, NULL, 0, NULL, NULL));
		opencl_assert(clFlush(cqCommandQueue));
	}

	void set_kernel_arg_mem(cl_kernel kernel, cl_uint *narg, const char *name)
	{
		cl_mem ptr;

		MemMap::iterator i = mem_map.find(name);
		if(i != mem_map.end()) {
			ptr = CL_MEM_PTR(i->second);
		}
		else {
			/* work around NULL not working, even though the spec says otherwise */
			ptr = CL_MEM_PTR(null_mem);
		}

		opencl_assert(clSetKernelArg(kernel, (*narg)++, sizeof(ptr), (void*)&ptr));
	}

	void film_convert(DeviceTask& task, device_ptr buffer, device_ptr rgba_byte, device_ptr rgba_half)
	{
		/* cast arguments to cl types */
		cl_mem d_data = CL_MEM_PTR(const_mem_map["__data"]->device_pointer);
		cl_mem d_rgba = (rgba_byte) ? CL_MEM_PTR(rgba_byte) : CL_MEM_PTR(rgba_half);
		cl_mem d_buffer = CL_MEM_PTR(buffer);
		cl_int d_x = task.x;
		cl_int d_y = task.y;
		cl_int d_w = task.w;
		cl_int d_h = task.h;
		cl_float d_sample_scale = 1.0f / (task.sample + 1);
		cl_int d_offset = task.offset;
		cl_int d_stride = task.stride;

		/* sample arguments */
		cl_uint narg = 0;

		cl_kernel ckFilmConvertKernel = (rgba_byte) ? ckFilmConvertByteKernel : ckFilmConvertHalfFloatKernel;

		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_rgba), (void*)&d_rgba));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_buffer), (void*)&d_buffer));

#define KERNEL_TEX(type, ttype, name) \
	set_kernel_arg_mem(ckFilmConvertKernel, &narg, #name);
#include "kernel_textures.h"
#undef KERNEL_TEX

		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_sample_scale), (void*)&d_sample_scale));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_x), (void*)&d_x));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_y), (void*)&d_y));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_w), (void*)&d_w));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_h), (void*)&d_h));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_offset), (void*)&d_offset));
		opencl_assert(clSetKernelArg(ckFilmConvertKernel, narg++, sizeof(d_stride), (void*)&d_stride));

		enqueue_kernel(ckFilmConvertKernel, d_w, d_h);
	}

	void shader(DeviceTask& task)
	{
		/* cast arguments to cl types */
		cl_mem d_data = CL_MEM_PTR(const_mem_map["__data"]->device_pointer);
		cl_mem d_input = CL_MEM_PTR(task.shader_input);
		cl_mem d_output = CL_MEM_PTR(task.shader_output);
		cl_int d_shader_eval_type = task.shader_eval_type;
		cl_int d_shader_x = task.shader_x;
		cl_int d_shader_w = task.shader_w;
		cl_int d_offset = task.offset;

		/* sample arguments */
		cl_uint narg = 0;

		cl_kernel kernel;

		if(task.shader_eval_type >= SHADER_EVAL_BAKE)
			kernel = ckBakeKernel;
		else
			kernel = ckShaderKernel;

		for(int sample = 0; sample < task.num_samples; sample++) {

			if(task.get_cancel())
				break;

			cl_int d_sample = sample;

			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_data), (void*)&d_data));
			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_input), (void*)&d_input));
			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_output), (void*)&d_output));

#define KERNEL_TEX(type, ttype, name) \
			set_kernel_arg_mem(kernel, &narg, #name);
#include "kernel_textures.h"
#undef KERNEL_TEX

			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_shader_eval_type), (void*)&d_shader_eval_type));
			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_shader_x), (void*)&d_shader_x));
			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_shader_w), (void*)&d_shader_w));
			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_offset), (void*)&d_offset));
			opencl_assert(clSetKernelArg(kernel, narg++, sizeof(d_sample), (void*)&d_sample));

			enqueue_kernel(kernel, task.shader_w, 1);

			task.update_progress(NULL);
		}
	}

};

class OpenCLDeviceMegaKernel : public OpenCLDeviceBase
{
public:
	DedicatedTaskPool task_pool;

	cl_kernel ckPathTraceKernel;
	cl_program path_trace_program;

	OpenCLDeviceMegaKernel(DeviceInfo& info, Stats &stats, bool background_)
	: OpenCLDeviceBase(info, stats, background_)
	{
		ckPathTraceKernel = NULL;
		path_trace_program = NULL;
	}

	bool load_kernels(bool /*experimental*/)
	{
		/* verify if device was initialized */
		if(!device_initialized) {
			fprintf(stderr, "OpenCL: failed to initialize device.\n");
			return false;
		}

		/* Get Shader, bake and film convert kernels */
		if(!OpenCLDeviceBase::load_kernels(false)) {
			return false;
		}

		/* try to use cached kernel */
		thread_scoped_lock cache_locker;
		path_trace_program = OpenCLCache::get_program(cpPlatform, cdDevice, OpenCLCache::OCL_DEV_MEGAKERNEL_PROGRAM, cache_locker);

		if(!path_trace_program) {
			/* verify we have right opencl version */
			if(!opencl_version_check())
				return false;

			/* md5 hash to detect changes */
			string kernel_path = path_get("kernel");
			string kernel_md5 = path_files_md5_hash(kernel_path);
			string custom_kernel_build_options = "-D__COMPILE_ONLY_MEGAKERNEL__ ";
			string device_md5 = device_md5_hash(custom_kernel_build_options);

			/* path to cached binary */
			string clbin = string_printf("cycles_kernel_%s_%s.clbin", device_md5.c_str(), kernel_md5.c_str());
			clbin = path_user_get(path_join("cache", clbin));

			/* path to preprocessed source for debugging */
			string clsrc, *debug_src = NULL;

			if(opencl_kernel_use_debug()) {
				clsrc = string_printf("cycles_kernel_%s_%s.cl", device_md5.c_str(), kernel_md5.c_str());
				clsrc = path_user_get(path_join("cache", clsrc));
				debug_src = &clsrc;
			}

			/* if exists already, try use it */
			if (path_exists(clbin) && load_binary(kernel_path, clbin, custom_kernel_build_options, &path_trace_program, debug_src)) {
				/* kernel loaded from binary */
			}
			else {

				string init_kernel_source = "#include \"kernel.cl\" // " + kernel_md5 + "\n";

				/* if does not exist or loading binary failed, compile kernel */
				if (!compile_kernel(kernel_path, init_kernel_source, custom_kernel_build_options, &path_trace_program, debug_src))
					return false;

				/* save binary for reuse */
				if(!save_binary(&path_trace_program, clbin))
					return false;
			}

			/* cache the program */
			OpenCLCache::store_program(cpPlatform, cdDevice, path_trace_program, OpenCLCache::OCL_DEV_MEGAKERNEL_PROGRAM, cache_locker);
		}

		/* find kernels */
		ckPathTraceKernel = clCreateKernel(path_trace_program, "kernel_ocl_path_trace", &ciErr);
		if(opencl_error(ciErr))
			return false;

		return true;
	}

	~OpenCLDeviceMegaKernel()
	{
		task_pool.stop();

		if(ckPathTraceKernel)
			clReleaseKernel(ckPathTraceKernel);

		if(path_trace_program)
			clReleaseProgram(path_trace_program);
	}

	void path_trace(RenderTile& rtile, int sample)
	{
		/* cast arguments to cl types */
		cl_mem d_data = CL_MEM_PTR(const_mem_map["__data"]->device_pointer);
		cl_mem d_buffer = CL_MEM_PTR(rtile.buffer);
		cl_mem d_rng_state = CL_MEM_PTR(rtile.rng_state);
		cl_int d_x = rtile.x;
		cl_int d_y = rtile.y;
		cl_int d_w = rtile.w;
		cl_int d_h = rtile.h;
		cl_int d_offset = rtile.offset;
		cl_int d_stride = rtile.stride;

		/* sample arguments */
		cl_int d_sample = sample;
		cl_uint narg = 0;

		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_buffer), (void*)&d_buffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_rng_state), (void*)&d_rng_state));

#define KERNEL_TEX(type, ttype, name) \
		set_kernel_arg_mem(ckPathTraceKernel, &narg, #name);
#include "kernel_textures.h"
#undef KERNEL_TEX

		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_sample), (void*)&d_sample));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_x), (void*)&d_x));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_y), (void*)&d_y));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_w), (void*)&d_w));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_h), (void*)&d_h));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_offset), (void*)&d_offset));
		opencl_assert(clSetKernelArg(ckPathTraceKernel, narg++, sizeof(d_stride), (void*)&d_stride));

		enqueue_kernel(ckPathTraceKernel, d_w, d_h);
	}

	void thread_run(DeviceTask *task)
	{
		if(task->type == DeviceTask::FILM_CONVERT) {
			film_convert(*task, task->buffer, task->rgba_byte, task->rgba_half);
		}
		else if(task->type == DeviceTask::SHADER) {
			shader(*task);
		}
		else if(task->type == DeviceTask::PATH_TRACE) {
			RenderTile tile;

			/* keep rendering tiles until done */
			while(task->acquire_tile(this, tile)) {
				int start_sample = tile.start_sample;
				int end_sample = tile.start_sample + tile.num_samples;

				for(int sample = start_sample; sample < end_sample; sample++) {
					if(task->get_cancel()) {
						if(task->need_finish_queue == false)
							break;
					}

					path_trace(tile, sample);

					tile.sample = sample + 1;

					task->update_progress(&tile);
				}

				/* Complete kernel execution before release tile */
				/* This helps in multi-device render;
				* The device that reaches the critical-section function release_tile
				* waits (stalling other devices from entering release_tile) for all kernels
				* to complete. If device1 (a slow-render device) reaches release_tile first then
				* it would stall device2 (a fast-render device) from proceeding to render next tile
				*/
				clFinish(cqCommandQueue);

				task->release_tile(tile);
			}
		}
	}

	class OpenCLDeviceTask : public DeviceTask {
	public:
		OpenCLDeviceTask(OpenCLDeviceMegaKernel *device, DeviceTask& task)
		: DeviceTask(task)
		{
			run = function_bind(&OpenCLDeviceMegaKernel::thread_run, device, this);
		}
	};

	int get_split_task_count(DeviceTask& /*task*/)
	{
		return 1;
	}

	void task_add(DeviceTask& task)
	{
		task_pool.push(new OpenCLDeviceTask(this, task));
	}

	void task_wait()
	{
		task_pool.wait();
	}

	void task_cancel()
	{
		task_pool.cancel();
	}
};

/* OpenCLDeviceSplitKernel's declaration/definition */
class OpenCLDeviceSplitKernel : public OpenCLDeviceBase
{
public:
	DedicatedTaskPool task_pool;

	/* Kernel declaration */
	cl_kernel ckPathTraceKernel_DataInit_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_LampEmission_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_DirectLighting_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL;
	cl_kernel ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL;

	/* cl_program declaration */
	cl_program dataInit_program;
	cl_program sceneIntersect_program;
	cl_program lampEmission_program;
	cl_program QueueEnqueue_program;
	cl_program background_BufferUpdate_program;
	cl_program shaderEval_program;
	cl_program holdout_emission_blurring_termination_ao_program;
	cl_program directLighting_program;
	cl_program shadowBlocked_program;
	cl_program nextIterationSetUp_program;
	cl_program sumAllRadiance_program;

	/* Global memory variables [porting]; These memory is used for
	* co-operation between different kernels; Data written by one
	* kernel will be avaible to another kernel via this global
	* memory
	*/
	cl_mem rng_coop;
	cl_mem throughput_coop;
	cl_mem L_transparent_coop;
	cl_mem PathRadiance_coop;
	cl_mem Ray_coop;
	cl_mem PathState_coop;
	cl_mem Intersection_coop;
	cl_mem kgbuffer; /* KernelGlobals buffer */

	/* global buffers for ShaderData */
	cl_mem sd;                      /* ShaderData used in the main path-iteration loop */
	cl_mem sd_DL_shadow;            /* ShaderData used in Direct Lighting and ShadowBlocked kernel */

	/* global buffers of each member of ShaderData */
	cl_mem P_sd;
	cl_mem P_sd_DL_shadow;
	cl_mem N_sd;
	cl_mem N_sd_DL_shadow;
	cl_mem Ng_sd;
	cl_mem Ng_sd_DL_shadow;
	cl_mem I_sd;
	cl_mem I_sd_DL_shadow;
	cl_mem shader_sd;
	cl_mem shader_sd_DL_shadow;
	cl_mem flag_sd;
	cl_mem flag_sd_DL_shadow;
	cl_mem prim_sd;
	cl_mem prim_sd_DL_shadow;
	cl_mem type_sd;
	cl_mem type_sd_DL_shadow;
	cl_mem u_sd;
	cl_mem u_sd_DL_shadow;
	cl_mem v_sd;
	cl_mem v_sd_DL_shadow;
	cl_mem object_sd;
	cl_mem object_sd_DL_shadow;
	cl_mem time_sd;
	cl_mem time_sd_DL_shadow;
	cl_mem ray_length_sd;
	cl_mem ray_length_sd_DL_shadow;
	cl_mem ray_depth_sd;
	cl_mem ray_depth_sd_DL_shadow;
	cl_mem transparent_depth_sd;
	cl_mem transparent_depth_sd_DL_shadow;
#ifdef __RAY_DIFFERENTIALS__
	cl_mem dP_sd, dI_sd;
	cl_mem dP_sd_DL_shadow, dI_sd_DL_shadow;
	cl_mem du_sd, dv_sd;
	cl_mem du_sd_DL_shadow, dv_sd_DL_shadow;
#endif
#ifdef __DPDU__
	cl_mem dPdu_sd, dPdv_sd;
	cl_mem dPdu_sd_DL_shadow, dPdv_sd_DL_shadow;
#endif
	cl_mem closure_sd;
	cl_mem closure_sd_DL_shadow;
	cl_mem num_closure_sd;
	cl_mem num_closure_sd_DL_shadow;
	cl_mem randb_closure_sd;
	cl_mem randb_closure_sd_DL_shadow;
	cl_mem ray_P_sd;
	cl_mem ray_P_sd_DL_shadow;
	cl_mem ray_dP_sd;
	cl_mem ray_dP_sd_DL_shadow;

	/* Global memory required for shadow blocked and accum_radiance */
	cl_mem BSDFEval_coop;
	cl_mem ISLamp_coop;
	cl_mem LightRay_coop;
	cl_mem AOAlpha_coop;
	cl_mem AOBSDF_coop;
	cl_mem AOLightRay_coop;
	cl_mem Intersection_coop_AO;
	cl_mem Intersection_coop_DL;

#ifdef WITH_CYCLES_DEBUG
	/* DebugData memory */
	cl_mem debugdata_coop;
#endif

	/* Global state array that tracks ray state */
	cl_mem ray_state;

	/* per sample buffers */
	cl_mem per_sample_output_buffers;

	/* Denotes which sample each ray is being processed for */
	cl_mem work_array;

	/* Queue*/
	cl_mem Queue_data;  /* Array of size queuesize * num_queues * sizeof(int) */
	cl_mem Queue_index; /* Array of size num_queues * sizeof(int); Tracks the size of each queue */

	/* Flag to make sceneintersect and lampemission kernel use queues */
	cl_mem use_queues_flag;

	/* Required-memory size */
	size_t rng_size;
	size_t throughput_size;
	size_t L_transparent_size;
	size_t rayState_size;
	size_t hostRayState_size;
	size_t work_element_size;
	size_t ISLamp_size;

	/* size of structures declared in kernel_types.h */
	size_t PathRadiance_size;
	size_t Ray_size;
	size_t PathState_size;
	size_t Intersection_size;

	/* Sizes of memory required for shadow blocked function */
	size_t AOAlpha_size;
	size_t AOBSDF_size;
	size_t AOLightRay_size;
	size_t LightRay_size;
	size_t BSDFEval_size;
	size_t Intersection_coop_AO_size;
	size_t Intersection_coop_DL_size;

#ifdef WITH_CYCLES_DEBUG
	size_t debugdata_size;
#endif

	/* Amount of memory in output buffer associated with one pixel/thread */
	size_t per_thread_output_buffer_size;

	/* Total allocatable available device memory */
	size_t total_allocatable_memory;

	/* host version of ray_state; Used in checking host path-iteration termination */
	char *hostRayStateArray;

	/* Number of path-iterations to be done in one shot */
	unsigned int PathIteration_times;

	/* Denotes if the render is background or foreground */
	bool background;

#ifdef __WORK_STEALING__
	/* Work pool with respect to each work group */
	cl_mem work_pool_wgs;

	/* Denotes the maximum work groups possible w.r.t. current tile size */
	unsigned int max_work_groups;
#endif

	/* clos_max value for which the kernels have been loaded currently */
	int current_clos_max;

	/* Marked True in constructor and marked false at the end of path_trace() */
	bool first_tile;

	OpenCLDeviceSplitKernel(DeviceInfo& info, Stats &stats, bool background_)
		: OpenCLDeviceBase(info, stats, background_)
	{

		this->info.use_split_kernel = true;
		background = background_;

		/* Initialize kernels */
		ckPathTraceKernel_DataInit_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_LampEmission_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_DirectLighting_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL = NULL;
		ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL = NULL;

		/* Initialize program */
		dataInit_program = NULL;
		sceneIntersect_program = NULL;
		lampEmission_program = NULL;
		QueueEnqueue_program = NULL;
		background_BufferUpdate_program = NULL;
		shaderEval_program = NULL;
		holdout_emission_blurring_termination_ao_program = NULL;
		directLighting_program = NULL;
		shadowBlocked_program = NULL;
		nextIterationSetUp_program = NULL;
		sumAllRadiance_program = NULL;

		/* Initialize cl_mem variables */
		kgbuffer = NULL;
		sd = NULL;
		sd_DL_shadow = NULL;

		P_sd = NULL;
		P_sd_DL_shadow = NULL;
		N_sd = NULL;
		N_sd_DL_shadow = NULL;
		Ng_sd = NULL;
		Ng_sd_DL_shadow = NULL;
		I_sd = NULL;
		I_sd_DL_shadow = NULL;
		shader_sd = NULL;
		shader_sd_DL_shadow = NULL;
		flag_sd = NULL;
		flag_sd_DL_shadow = NULL;
		prim_sd = NULL;
		prim_sd_DL_shadow = NULL;
		type_sd = NULL;
		type_sd_DL_shadow = NULL;
		u_sd = NULL;
		u_sd_DL_shadow = NULL;
		v_sd = NULL;
		v_sd_DL_shadow = NULL;
		object_sd = NULL;
		object_sd_DL_shadow = NULL;
		time_sd = NULL;
		time_sd_DL_shadow = NULL;
		ray_length_sd = NULL;
		ray_length_sd_DL_shadow = NULL;
		ray_depth_sd = NULL;
		ray_depth_sd_DL_shadow = NULL;
		transparent_depth_sd = NULL;
		transparent_depth_sd_DL_shadow = NULL;
#ifdef __RAY_DIFFERENTIALS__
		dP_sd = NULL;
		dI_sd = NULL;
		dP_sd_DL_shadow = NULL;
		dI_sd_DL_shadow = NULL;
		du_sd = NULL;
		dv_sd = NULL;
		du_sd_DL_shadow = NULL;
		dv_sd_DL_shadow = NULL;
#endif
#ifdef __DPDU__
		dPdu_sd = NULL;
		dPdv_sd = NULL;
		dPdu_sd_DL_shadow = NULL;
		dPdv_sd_DL_shadow = NULL;
#endif
		closure_sd = NULL;
		closure_sd_DL_shadow = NULL;
		num_closure_sd = NULL;
		num_closure_sd_DL_shadow = NULL;
		randb_closure_sd = NULL;
		randb_closure_sd_DL_shadow = NULL;
		ray_P_sd = NULL;
		ray_P_sd_DL_shadow = NULL;
		ray_dP_sd = NULL;
		ray_dP_sd_DL_shadow = NULL;

		rng_coop = NULL;
		throughput_coop = NULL;
		L_transparent_coop = NULL;
		PathRadiance_coop = NULL;
		Ray_coop = NULL;
		PathState_coop = NULL;
		Intersection_coop = NULL;
		ray_state = NULL;

		AOAlpha_coop = NULL;
		AOBSDF_coop = NULL;
		AOLightRay_coop = NULL;
		BSDFEval_coop = NULL;
		ISLamp_coop = NULL;
		LightRay_coop = NULL;
		Intersection_coop_AO = NULL;
		Intersection_coop_DL = NULL;

#ifdef WITH_CYCLES_DEBUG
		debugdata_coop = NULL;
#endif

		work_array = NULL;

		/* Queue */
		Queue_data = NULL;
		Queue_index = NULL;
		use_queues_flag = NULL;

		per_sample_output_buffers = NULL;

		/* Initialize required memory size */
		rng_size = sizeof(RNG);
		throughput_size = sizeof(float3);
		L_transparent_size = sizeof(float);
		rayState_size = sizeof(char);
		hostRayState_size = sizeof(char);
		work_element_size = sizeof(unsigned int);
		ISLamp_size = sizeof(int);

		/* Initialize size of structures declared in kernel_types.h */
		PathRadiance_size = sizeof(PathRadiance);
		Ray_size = sizeof(Ray);
		PathState_size = sizeof(PathState);
		Intersection_size = sizeof(Intersection);

		/* Initialize sizes of memory required for shadow blocked function */
		LightRay_size = sizeof(Ray);
		BSDFEval_size = sizeof(BsdfEval);
		Intersection_coop_AO_size = sizeof(Intersection);
		Intersection_coop_DL_size = sizeof(Intersection);

		/* Initialize sizes of memory required for shadow blocked function */
		AOAlpha_size = sizeof(float3);
		AOBSDF_size = sizeof(float3);
		AOLightRay_size = sizeof(Ray);
		LightRay_size = sizeof(Ray);
		BSDFEval_size = sizeof(BsdfEval);
		Intersection_coop_AO_size = sizeof(Intersection);
		Intersection_coop_DL_size = sizeof(Intersection);

#ifdef WITH_CYCLES_DEBUG
		debugdata_size = sizeof(DebugData);
#endif

		per_thread_output_buffer_size = 0;
		hostRayStateArray = NULL;
		PathIteration_times = PATH_ITER_INC_FACTOR;
#ifdef __WORK_STEALING__
		work_pool_wgs = NULL;
		max_work_groups = 0;
#endif
		current_clos_max = -1;
		first_tile = true;

		/* Get device's maximum memory that can be allocated */
		ciErr = clGetDeviceInfo(cdDevice, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(size_t), &total_allocatable_memory, NULL);
		assert(ciErr == CL_SUCCESS);
		if(platform_name == "AMD Accelerated Parallel Processing") {
			/* This value is tweak-able; AMD platform does not seem to
			* give maximum performance when all of CL_DEVICE_MAX_MEM_ALLOC_SIZE
			* is considered for further computation.
			*/
			total_allocatable_memory /= 2;
		}
	}

	bool load_split_kernel(string kernel_path,
		string kernel_init_source,
		string clbin,
		string custom_kernel_build_options,
		cl_program *program) {

		if(!opencl_version_check())
			return false;

		clbin = path_user_get(path_join("cache", clbin));

		/* path to preprocessed source for debugging */
		string *debug_src = NULL;

		/* if exists already, try use it */
		if (path_exists(clbin) && load_binary(kernel_path, clbin, custom_kernel_build_options, program, debug_src)) {
			/* kernel loaded from binary */
		}
		else {
			/* if does not exist or loading binary failed, compile kernel */
			if (!compile_kernel(kernel_path, kernel_init_source, custom_kernel_build_options, program))
				return false;

			/* save binary for reuse */
			if(!save_binary(program, clbin))
				return false;
		}

		return true;
	}

	/* Split kernel utility functions */
	size_t get_tex_size(const char *tex_name) {
		cl_mem ptr;
		size_t ret_size;

		MemMap::iterator i = mem_map.find(tex_name);
		if(i != mem_map.end()) {
			ptr = CL_MEM_PTR(i->second);
			ciErr = clGetMemObjectInfo(ptr, CL_MEM_SIZE, sizeof(ret_size), &ret_size, NULL);
			assert(ciErr == CL_SUCCESS);
		}
		else {
			ret_size = 0;
		}

		return ret_size;
	}

	size_t get_shader_closure_size(int max_closure) {
		return (sizeof(ShaderClosure)* max_closure);
	}

	size_t get_shader_data_size(size_t shader_closure_size) {
		/* ShaderData size without accounting for ShaderClosure array */
		size_t shader_data_size = sizeof(ShaderData) - (sizeof(ShaderClosure) * MAX_CLOSURE);
		return (shader_data_size + shader_closure_size);
	}

	/* Returns size of KernelGlobals structure associated with OpenCL */
	size_t get_KernelGlobals_size() {
		/* Copy dummy KernelGlobals related to OpenCL from kernel_globals.h to fetch its size */
		typedef struct KernelGlobals {
			ccl_constant KernelData *data;
#define KERNEL_TEX(type, ttype, name) \
	ccl_global type *name;
#include "kernel_textures.h"
#undef KERNEL_TEX
		} KernelGlobals;

		return sizeof(KernelGlobals);
	}

	/* Returns size of Structure of arrays implementation of */
	size_t get_shaderdata_soa_size() {

		size_t shader_soa_size = 0;

#define SD_VAR(type, what) \
		shader_soa_size += sizeof(void *);
#define SD_CLOSURE_VAR(type, what, max_closure)
		shader_soa_size += sizeof(void *);
		#include "kernel_shaderdata_vars.h"
#undef SD_VAR
#undef SD_CLOSURE_VAR

		return shader_soa_size;
	}

	/* Get enum type names */
	/* ToDo: Avoid this switch somehow */
	string get_node_type_as_string(NodeType node) {
		switch (node) {
		case NODE_SHADER_JUMP: return "__NODE_SHADER_JUMP__";
		case NODE_CLOSURE_BSDF: return "__NODE_CLOSURE_BSDF__";
		case NODE_CLOSURE_EMISSION: return "__NODE_CLOSURE_EMISSION__";
		case NODE_CLOSURE_BACKGROUND: return "__NODE_CLOSURE_BACKGROUND__";
		case NODE_CLOSURE_HOLDOUT: return "__NODE_CLOSURE_HOLDOUT__";
		case NODE_CLOSURE_AMBIENT_OCCLUSION: return "__NODE_CLOSURE_AMBIENT_OCCLUSION__";
		case NODE_CLOSURE_VOLUME: return "__NODE_CLOSURE_VOLUME__";
		case NODE_CLOSURE_SET_WEIGHT: return "__NODE_CLOSURE_SET_WEIGHT__";
		case NODE_CLOSURE_WEIGHT: return "__NODE_CLOSURE_WEIGHT__";
		case NODE_EMISSION_WEIGHT: return "__NODE_EMISSION_WEIGHT__";
		case NODE_MIX_CLOSURE: return "__NODE_MIX_CLOSURE__";
		case NODE_JUMP_IF_ZERO: return "__NODE_JUMP_IF_ZERO__";
		case NODE_JUMP_IF_ONE: return "__NODE_JUMP_IF_ONE__";
		case NODE_TEX_IMAGE: return "__NODE_TEX_IMAGE__";
		case NODE_TEX_IMAGE_BOX: return "__NODE_TEX_IMAGE_BOX__";
		case NODE_TEX_ENVIRONMENT: return "__NODE_TEX_ENVIRONMENT__";
		case NODE_TEX_SKY: return "__NODE_TEX_SKY__";
		case NODE_TEX_GRADIENT: return "__NODE_TEX_GRADIENT__";
		case NODE_TEX_NOISE: return "__NODE_TEX_NOISE__";
		case NODE_TEX_VORONOI: return "__NODE_TEX_VORONOI__";
		case NODE_TEX_MUSGRAVE: return "__NODE_TEX_MUSGRAVE__";
		case NODE_TEX_WAVE: return "__NODE_TEX_WAVE__";
		case NODE_TEX_MAGIC: return "__NODE_TEX_MAGIC__";
		case NODE_TEX_CHECKER: return "__NODE_TEX_CHECKER__";
		case NODE_TEX_BRICK: return "__NODE_TEX_BRICK__";
		case NODE_CAMERA: return "__NODE_CAMERA__";
		case NODE_GEOMETRY: return "__NODE_GEOMETRY__";
		case NODE_GEOMETRY_BUMP_DX: return "__NODE_GEOMETRY_BUMP_DX__";
		case NODE_GEOMETRY_BUMP_DY: return "__NODE_GEOMETRY_BUMP_DY__";
		case NODE_LIGHT_PATH: return "__NODE_LIGHT_PATH__";
		case NODE_OBJECT_INFO: return "__NODE_OBJECT_INFO__";
		case NODE_PARTICLE_INFO: return "__NODE_PARTICLE_INFO__";
		case NODE_HAIR_INFO: return "__NODE_HAIR_INFO__";
		case NODE_CONVERT: return "__NODE_CONVERT__";
		case NODE_VALUE_F: return "__NODE_VALUE_F__";
		case NODE_VALUE_V: return "__NODE_VALUE_V__";
		case NODE_INVERT: return "__NODE_INVERT__";
		case NODE_GAMMA: return "__NODE_GAMMA__";
		case NODE_BRIGHTCONTRAST: return "__NODE_BRIGHTCONTRAST__";
		case NODE_MIX: return "__NODE_MIX__";
		case NODE_SEPARATE_VECTOR: return "__NODE_SEPARATE_VECTOR__";
		case NODE_COMBINE_VECTOR: return "__NODE_COMBINE_VECTOR__";
		case NODE_SEPARATE_HSV: return "__NODE_SEPARATE_HSV__";
		case NODE_COMBINE_HSV: return "__NODE_COMBINE_HSV__";
		case NODE_HSV: return "__NODE_HSV__";
		case NODE_ATTR: return "__NODE_ATTR__";
		case NODE_ATTR_BUMP_DX: return "__NODE_ATTR_BUMP_DX__";
		case NODE_ATTR_BUMP_DY: return "__NODE_ATTR_BUMP_DY__";
		case NODE_FRESNEL: return "__NODE_FRESNEL__";
		case NODE_LAYER_WEIGHT: return "__NODE_LAYER_WEIGHT__";
		case NODE_WIREFRAME: return "__NODE_WIREFRAME__";
		case NODE_WAVELENGTH: return "__NODE_WAVELENGTH__";
		case NODE_BLACKBODY: return "__NODE_BLACKBODY__";
		case NODE_SET_DISPLACEMENT: return "__NODE_SET_DISPLACEMENT__";
		case NODE_SET_BUMP: return "__NODE_SET_BUMP__";
		case NODE_MATH: return "__NODE_MATH__";
		case NODE_VECTOR_MATH: return "__NODE_VECTOR_MATH__";
		case NODE_VECTOR_TRANSFORM: return "__NODE_VECTOR_TRANSFORM__";
		case NODE_NORMAL: return "__NODE_NORMAL__";
		case NODE_MAPPING: return "__NODE_MAPPING__";
		case NODE_MIN_MAX: return "__NODE_MIN_MAX__";
		case NODE_TEX_COORD: return "__NODE_TEX_COORD__";
		case NODE_TEX_COORD_BUMP_DX: return "__NODE_TEX_COORD_BUMP_DX__";
		case NODE_TEX_COORD_BUMP_DY: return "__NODE_TEX_COORD_BUMP_DY__";
		case NODE_CLOSURE_SET_NORMAL: return "__NODE_CLOSURE_SET_NORMAL__";
		case NODE_RGB_RAMP: return "__NODE_RGB_RAMP__";
		case NODE_RGB_CURVES: return "__NODE_RGB_CURVES__";
		case NODE_VECTOR_CURVES: return "__NODE_VECTOR_CURVES__";
		case NODE_LIGHT_FALLOFF: return "__NODE_LIGHT_FALLOFF__";
		case NODE_TANGENT: return "__NODE_TANGENT__";
		case NODE_NORMAL_MAP: return "__NODE_NORMAL_MAP__";
		case NODE_END: return "__NODE_END__";
		default:
			assert(!"Unknown node type passed");
		}
		return "";
	}

	bool load_kernels(bool /*experimental*/)
	{
		/* verify if device was initialized */
		if(!device_initialized) {
			fprintf(stderr, "OpenCL: failed to initialize device.\n");
			return false;
		}

		/* if it is an interactive render; we ceil clos_max value to a multiple of 5 in order
		* to limit re-compilations
		*/
		if(!background) {
			assert((clos_max != 0) && "clos_max value is 0" );
			clos_max = (((clos_max - 1) / 5) + 1) * 5;
			/* clos_max value shouldn't be greater than MAX_CLOSURE */
			clos_max = (clos_max > MAX_CLOSURE) ? MAX_CLOSURE : clos_max;

			if(current_clos_max == clos_max) {
				/* present kernels have been created with the same closure count build option */
				return true;
			}
		}

		/* Get Shader, bake and film_convert kernels */
		if(!OpenCLDeviceBase::load_kernels(false)) {
			return false;
		}

		string svm_build_options = "";
		string max_closure_build_option = "";
		string compute_device_type_build_option = "";

		/* Set svm_build_options */
		/* Enable only the macros related to the scene */
		for(int node_iter = NODE_END; node_iter <= NODE_UVMAP; node_iter++) {
			if(node_iter == NODE_GEOMETRY_DUPLI || node_iter == NODE_UVMAP) { continue; }
			if(closure_nodes.find(node_iter) != closure_nodes.end()) {
				svm_build_options += " -D" + get_node_type_as_string((NodeType)node_iter);
			}
		}
		svm_build_options += " ";
		/* Set max closure build option */
#ifdef __MULTI_CLOSURE__
		max_closure_build_option += string_printf("-DMAX_CLOSURE=%d ", clos_max);
#endif

		/* Set compute device build option */
		cl_device_type device_type;
		ciErr = clGetDeviceInfo(cdDevice, CL_DEVICE_TYPE, sizeof(cl_device_type), &device_type, NULL);
		assert(ciErr == CL_SUCCESS);
		if(device_type == CL_DEVICE_TYPE_GPU) {
			compute_device_type_build_option = "-D__COMPUTE_DEVICE_GPU__ ";
		}

		string kernel_path = path_get("kernel");
		string kernel_md5 = path_files_md5_hash(kernel_path);
		string device_md5;
		string custom_kernel_build_options;
		string kernel_init_source;
		string clbin;

		string common_custom_build_options = "";
		common_custom_build_options = "-D__SPLIT_KERNEL__ " + max_closure_build_option;
#ifdef __WORK_STEALING__
		common_custom_build_options += "-D__WORK_STEALING__ ";
#endif

		kernel_init_source = "#include \"kernel_DataInit.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_DataInit.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &dataInit_program))
			return false;

		kernel_init_source = "#include \"kernel_SceneIntersect.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_SceneIntersect.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &sceneIntersect_program))
			return false;

		kernel_init_source = "#include \"kernel_LampEmission.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options + svm_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_LampEmission.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &lampEmission_program))
			return false;

		kernel_init_source = "#include \"kernel_QueueEnqueue.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_QueueEnqueue.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &QueueEnqueue_program))
			return false;

		kernel_init_source = "#include \"kernel_Background_BufferUpdate.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options + compute_device_type_build_option + svm_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_Background_BufferUpdate.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &background_BufferUpdate_program))
			return false;

		kernel_init_source = "#include \"kernel_ShaderEval.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options + svm_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_ShaderEval.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &shaderEval_program))
			return false;

		kernel_init_source = "#include \"kernel_Holdout_Emission_Blurring_Pathtermination_AO.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options + compute_device_type_build_option;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_Holdout_Emission_Blurring_Pathtermination_AO.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &holdout_emission_blurring_termination_ao_program))
			return false;

		kernel_init_source = "#include \"kernel_DirectLighting.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options + compute_device_type_build_option + svm_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_DirectLighting.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &directLighting_program))
			return false;

		kernel_init_source = "#include \"kernel_ShadowBlocked.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options + svm_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_ShadowBlocked.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &shadowBlocked_program))
			return false;

		kernel_init_source = "#include \"kernel_NextIterationSetUp.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options + compute_device_type_build_option;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_NextIterationSetUp.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &nextIterationSetUp_program))
			return false;

		kernel_init_source = "#include \"kernel_SumAllRadiance.cl\" // " + kernel_md5 + "\n";
		custom_kernel_build_options = common_custom_build_options;
		device_md5 = device_md5_hash(custom_kernel_build_options);
		clbin = string_printf("cycles_kernel_%s_%s_SumAllRadiance.clbin", device_md5.c_str(), kernel_md5.c_str());
		if(!load_split_kernel(kernel_path, kernel_init_source, clbin, custom_kernel_build_options, &sumAllRadiance_program))
			return false;

		current_clos_max = clos_max;

		/* find kernels */
		ckPathTraceKernel_DataInit_SPLIT_KERNEL = clCreateKernel(dataInit_program, "kernel_ocl_path_trace_data_initialization_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL = clCreateKernel(sceneIntersect_program, "kernel_ocl_path_trace_SceneIntersect_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_LampEmission_SPLIT_KERNEL = clCreateKernel(lampEmission_program, "kernel_ocl_path_trace_LampEmission_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL = clCreateKernel(QueueEnqueue_program, "kernel_ocl_path_trace_QueueEnqueue_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL = clCreateKernel(background_BufferUpdate_program, "kernel_ocl_path_trace_Background_BufferUpdate_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL = clCreateKernel(shaderEval_program, "kernel_ocl_path_trace_ShaderEvaluation_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL = clCreateKernel(holdout_emission_blurring_termination_ao_program, "kernel_ocl_path_trace_holdout_emission_blurring_pathtermination_AO_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_DirectLighting_SPLIT_KERNEL = clCreateKernel(directLighting_program, "kernel_ocl_path_trace_DirectLighting_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL = clCreateKernel(shadowBlocked_program, "kernel_ocl_path_trace_ShadowBlocked_DirectLighting_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL = clCreateKernel(nextIterationSetUp_program, "kernel_ocl_path_trace_SetupNextIteration_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL = clCreateKernel(sumAllRadiance_program, "kernel_ocl_path_trace_SumAllRadiance_SPLIT_KERNEL", &ciErr);
		if(opencl_error(ciErr))
			return false;

		return true;
	}

	~OpenCLDeviceSplitKernel()
	{
		task_pool.stop();

		/* Release kernels */
		if(ckPathTraceKernel_DataInit_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_DataInit_SPLIT_KERNEL);

		if(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL);

		if(ckPathTraceKernel_LampEmission_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_LampEmission_SPLIT_KERNEL);

		if(ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL);

		if(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL);

		if(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL);

		if(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL);

		if(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL);

		if(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL);

		if(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL);

		if(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL)
			clReleaseKernel(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL);

		/* Release global memory */
		if(P_sd != NULL)
			clReleaseMemObject(P_sd);

		if(P_sd_DL_shadow != NULL)
			clReleaseMemObject(P_sd_DL_shadow);

		if(N_sd != NULL)
			clReleaseMemObject(N_sd);

		if(N_sd_DL_shadow != NULL)
			clReleaseMemObject(N_sd_DL_shadow);

		if(Ng_sd != NULL)
			clReleaseMemObject(Ng_sd);

		if(Ng_sd_DL_shadow != NULL)
			clReleaseMemObject(Ng_sd_DL_shadow);

		if(I_sd != NULL)
			clReleaseMemObject(I_sd);

		if(I_sd_DL_shadow != NULL)
			clReleaseMemObject(I_sd_DL_shadow);

		if(shader_sd != NULL)
			clReleaseMemObject(shader_sd);

		if(shader_sd_DL_shadow != NULL)
			clReleaseMemObject(shader_sd_DL_shadow);

		if(flag_sd != NULL)
			clReleaseMemObject(flag_sd);

		if(flag_sd_DL_shadow != NULL)
			clReleaseMemObject(flag_sd_DL_shadow);

		if(prim_sd != NULL)
			clReleaseMemObject(prim_sd);

		if(prim_sd_DL_shadow != NULL)
			clReleaseMemObject(prim_sd_DL_shadow);

		if(type_sd != NULL)
			clReleaseMemObject(type_sd);

		if(type_sd_DL_shadow != NULL)
			clReleaseMemObject(type_sd_DL_shadow);

		if(u_sd != NULL)
			clReleaseMemObject(u_sd);

		if(u_sd_DL_shadow != NULL)
			clReleaseMemObject(u_sd_DL_shadow);

		if(v_sd != NULL)
			clReleaseMemObject(v_sd);

		if(v_sd_DL_shadow != NULL)
			clReleaseMemObject(v_sd_DL_shadow);

		if(object_sd != NULL)
			clReleaseMemObject(object_sd);

		if(object_sd_DL_shadow != NULL)
			clReleaseMemObject(object_sd_DL_shadow);

		if(time_sd != NULL)
			clReleaseMemObject(time_sd);

		if(time_sd_DL_shadow != NULL)
			clReleaseMemObject(time_sd_DL_shadow);

		if(ray_length_sd != NULL)
			clReleaseMemObject(ray_length_sd);

		if(ray_length_sd_DL_shadow != NULL)
			clReleaseMemObject(ray_length_sd_DL_shadow);

		if(ray_depth_sd != NULL)
			clReleaseMemObject(ray_depth_sd);

		if(ray_depth_sd_DL_shadow != NULL)
			clReleaseMemObject(ray_depth_sd_DL_shadow);

		if(transparent_depth_sd != NULL)
			clReleaseMemObject(transparent_depth_sd);

		if(transparent_depth_sd_DL_shadow != NULL)
			clReleaseMemObject(transparent_depth_sd_DL_shadow);

#ifdef __RAY_DIFFERENTIALS__
		if(dP_sd != NULL)
			clReleaseMemObject(dP_sd);

		if(dP_sd_DL_shadow != NULL)
			clReleaseMemObject(dP_sd_DL_shadow);

		if(dI_sd != NULL)
			clReleaseMemObject(dI_sd);

		if(dI_sd_DL_shadow != NULL)
			clReleaseMemObject(dI_sd_DL_shadow);

		if(du_sd != NULL)
			clReleaseMemObject(du_sd);

		if(du_sd_DL_shadow != NULL)
			clReleaseMemObject(du_sd_DL_shadow);

		if(dv_sd != NULL)
			clReleaseMemObject(dv_sd);

		if(dv_sd_DL_shadow != NULL)
			clReleaseMemObject(dv_sd_DL_shadow);
#endif
#ifdef __DPDU__
		if(dPdu_sd != NULL)
			clReleaseMemObject(dPdu_sd);

		if(dPdu_sd_DL_shadow != NULL)
			clReleaseMemObject(dPdu_sd_DL_shadow);

		if(dPdv_sd != NULL)
			clReleaseMemObject(dPdv_sd);

		if(dPdv_sd_DL_shadow != NULL)
			clReleaseMemObject(dPdv_sd_DL_shadow);
#endif

		if(closure_sd != NULL)
			clReleaseMemObject(closure_sd);

		if(closure_sd_DL_shadow != NULL)
			clReleaseMemObject(closure_sd_DL_shadow);

		if(num_closure_sd != NULL)
			clReleaseMemObject(num_closure_sd);

		if(num_closure_sd_DL_shadow != NULL)
			clReleaseMemObject(num_closure_sd_DL_shadow);

		if(randb_closure_sd != NULL)
			clReleaseMemObject(randb_closure_sd);

		if(randb_closure_sd_DL_shadow != NULL)
			clReleaseMemObject(randb_closure_sd_DL_shadow);

		if(ray_P_sd != NULL)
			clReleaseMemObject(ray_P_sd);

		if(ray_P_sd_DL_shadow != NULL)
			clReleaseMemObject(ray_P_sd_DL_shadow);

		if(ray_dP_sd != NULL)
			clReleaseMemObject(ray_dP_sd);

		if(ray_dP_sd_DL_shadow != NULL)
			clReleaseMemObject(ray_dP_sd_DL_shadow);

		if(rng_coop != NULL)
			clReleaseMemObject(rng_coop);

		if(throughput_coop != NULL)
			clReleaseMemObject(throughput_coop);

		if(L_transparent_coop != NULL)
			clReleaseMemObject(L_transparent_coop);

		if(PathRadiance_coop != NULL)
			clReleaseMemObject(PathRadiance_coop);

		if(Ray_coop != NULL)
			clReleaseMemObject(Ray_coop);

		if(PathState_coop != NULL)
			clReleaseMemObject(PathState_coop);

		if(Intersection_coop != NULL)
			clReleaseMemObject(Intersection_coop);

		if(kgbuffer != NULL)
			clReleaseMemObject(kgbuffer);

		if(sd != NULL)
			clReleaseMemObject(sd);

		if(sd_DL_shadow != NULL)
			clReleaseMemObject(sd_DL_shadow);

		if(ray_state != NULL)
			clReleaseMemObject(ray_state);

		if(AOAlpha_coop != NULL)
			clReleaseMemObject(AOAlpha_coop);

		if(AOBSDF_coop != NULL)
			clReleaseMemObject(AOBSDF_coop);

		if(AOLightRay_coop != NULL)
			clReleaseMemObject(AOLightRay_coop);

		if(BSDFEval_coop != NULL)
			clReleaseMemObject(BSDFEval_coop);

		if(ISLamp_coop != NULL)
			clReleaseMemObject(ISLamp_coop);

		if(LightRay_coop != NULL)
			clReleaseMemObject(LightRay_coop);

		if(Intersection_coop_AO != NULL)
			clReleaseMemObject(Intersection_coop_AO);

		if(Intersection_coop_DL != NULL)
			clReleaseMemObject(Intersection_coop_DL);

#ifdef WITH_CYCLES_DEBUG
		if(debugdata_coop != NULL)
			clReleaseMemObject(debugdata_coop);
#endif

		if(use_queues_flag != NULL)
			clReleaseMemObject(use_queues_flag);

		if(Queue_data != NULL)
			clReleaseMemObject(Queue_data);

		if(Queue_index != NULL)
			clReleaseMemObject(Queue_index);

		if(work_array != NULL)
			clReleaseMemObject(work_array);

#ifdef __WORK_STEALING__
		if(work_pool_wgs != NULL)
			clReleaseMemObject(work_pool_wgs);
#endif

		if(per_sample_output_buffers != NULL)
			clReleaseMemObject(per_sample_output_buffers);

		if(hostRayStateArray != NULL)
			free(hostRayStateArray);

		/* Release programs */
		if(dataInit_program)
			clReleaseProgram(dataInit_program);

		if(sceneIntersect_program)
			clReleaseProgram(sceneIntersect_program);

		if(lampEmission_program)
			clReleaseProgram(lampEmission_program);

		if(QueueEnqueue_program)
			clReleaseProgram(QueueEnqueue_program);

		if(background_BufferUpdate_program)
			clReleaseProgram(background_BufferUpdate_program);

		if(shaderEval_program)
			clReleaseProgram(shaderEval_program);

		if(holdout_emission_blurring_termination_ao_program)
			clReleaseProgram(holdout_emission_blurring_termination_ao_program);

		if(directLighting_program)
			clReleaseProgram(directLighting_program);

		if(shadowBlocked_program)
			clReleaseProgram(shadowBlocked_program);

		if(nextIterationSetUp_program)
			clReleaseProgram(nextIterationSetUp_program);

		if(sumAllRadiance_program)
			clReleaseProgram(sumAllRadiance_program);
	}

	void path_trace(RenderTile& rtile, int2 max_render_feasible_tile_size)
	{
		/* cast arguments to cl types */
		cl_mem d_data = CL_MEM_PTR(const_mem_map["__data"]->device_pointer);
		cl_mem d_buffer = CL_MEM_PTR(rtile.buffer);
		cl_mem d_rng_state = CL_MEM_PTR(rtile.rng_state);
		cl_int d_x = rtile.x;
		cl_int d_y = rtile.y;
		cl_int d_w = rtile.w;
		cl_int d_h = rtile.h;
		cl_int d_offset = rtile.offset;
		cl_int d_stride = rtile.stride;

		/* Make sure that set render feasible tile size is a multiple of local work size dimensions */
		assert(max_render_feasible_tile_size.x % SPLIT_KERNEL_LOCAL_SIZE_X == 0);
		assert(max_render_feasible_tile_size.y % SPLIT_KERNEL_LOCAL_SIZE_Y == 0);

		/* ray_state and hostRayStateArray should be of same size */
		assert(hostRayState_size == rayState_size);
		assert(rayState_size == 1);

		size_t global_size[2];
		size_t local_size[2] = { SPLIT_KERNEL_LOCAL_SIZE_X, SPLIT_KERNEL_LOCAL_SIZE_Y };

		/* Set the range of samples to be processed for every ray in path-regeneration logic */
		cl_int start_sample = rtile.start_sample;
		cl_int end_sample = rtile.start_sample + rtile.num_samples;
		cl_int num_samples = rtile.num_samples;

#ifdef __WORK_STEALING__
		global_size[0] = (((d_w - 1) / local_size[0]) + 1) * local_size[0];
		global_size[1] = (((d_h - 1) / local_size[1]) + 1) * local_size[1];
		unsigned int num_parallel_samples = 1;
#else
		global_size[1] = (((d_h - 1) / local_size[1]) + 1) * local_size[1];
		unsigned int num_threads = max_render_feasible_tile_size.x * max_render_feasible_tile_size.y;
		unsigned int num_tile_columns_possible = num_threads / global_size[1];
		/* Estimate number of parallel samples that can be processed in parallel */
		unsigned int num_parallel_samples = (num_tile_columns_possible / d_w) <= rtile.num_samples ? (num_tile_columns_possible / d_w) : rtile.num_samples;
		/* Wavefront size in AMD is 64 */
		num_parallel_samples = ((num_parallel_samples / 64) == 0) ?
		num_parallel_samples :
							 (num_parallel_samples / 64) * 64;
		assert(num_parallel_samples != 0);

		global_size[0] = d_w * num_parallel_samples;
#endif // __WORK_STEALING__

		assert(global_size[0] * global_size[1] <= max_render_feasible_tile_size.x * max_render_feasible_tile_size.y);

		/* Allocate all required global memory once */
		if(first_tile) {
			size_t num_global_elements = max_render_feasible_tile_size.x * max_render_feasible_tile_size.y;

#ifdef __MULTI_CLOSURE__
			size_t ShaderClosure_size = get_shader_closure_size(clos_max);
#else
			size_t ShaderClosure_size = get_shader_closure_size(MAX_CLOSURE);
#endif

#ifdef __WORK_STEALING__
			/* Calculate max groups */
			size_t max_global_size[2];
			size_t tile_x = max_render_feasible_tile_size.x;
			size_t tile_y = max_render_feasible_tile_size.y;
			max_global_size[0] = (((tile_x - 1) / local_size[0]) + 1) * local_size[0];
			max_global_size[1] = (((tile_y - 1) / local_size[1]) + 1) * local_size[1];
			max_work_groups = (max_global_size[0] * max_global_size[1]) / (local_size[0] * local_size[1]);

			/* Allocate work_pool_wgs memory */
			work_pool_wgs = mem_alloc(max_work_groups * sizeof(unsigned int));
#endif

			/* Allocate queue_index memory only once */
			Queue_index = mem_alloc(NUM_QUEUES * sizeof(int));
			use_queues_flag = mem_alloc(sizeof(char));
			kgbuffer = mem_alloc(get_KernelGlobals_size());

			/* Create global buffers for ShaderData */
			sd = mem_alloc(get_shaderdata_soa_size());
			sd_DL_shadow = mem_alloc(get_shaderdata_soa_size());
			P_sd = mem_alloc(num_global_elements * sizeof(float3));
			P_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float3));
			N_sd = mem_alloc(num_global_elements * sizeof(float3));
			N_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float3));
			Ng_sd = mem_alloc(num_global_elements * sizeof(float3));
			Ng_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float3));
			I_sd = mem_alloc(num_global_elements * sizeof(float3));
			I_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float3));
			shader_sd = mem_alloc(num_global_elements * sizeof(int));
			shader_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));
			flag_sd = mem_alloc(num_global_elements * sizeof(int));
			flag_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));
			prim_sd = mem_alloc(num_global_elements * sizeof(int));
			prim_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));
			type_sd = mem_alloc(num_global_elements * sizeof(int));
			type_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));
			u_sd = mem_alloc(num_global_elements * sizeof(float));
			u_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float));
			v_sd = mem_alloc(num_global_elements * sizeof(float));
			v_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float));
			object_sd = mem_alloc(num_global_elements * sizeof(int));
			object_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));
			time_sd = mem_alloc(num_global_elements * sizeof(float));
			time_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float));
			ray_length_sd = mem_alloc(num_global_elements * sizeof(float));
			ray_length_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float));
			ray_depth_sd = mem_alloc(num_global_elements * sizeof(int));
			ray_depth_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));
			transparent_depth_sd = mem_alloc(num_global_elements * sizeof(int));
			transparent_depth_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));

#ifdef __RAY_DIFFERENTIALS__
			dP_sd = mem_alloc(num_global_elements * sizeof(differential3));
			dP_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(differential3));
			dI_sd = mem_alloc(num_global_elements * sizeof(differential3));
			dI_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(differential3));
			du_sd = mem_alloc(num_global_elements * sizeof(differential));
			du_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(differential));
			dv_sd = mem_alloc(num_global_elements * sizeof(differential));
			dv_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(differential));
#endif

#ifdef __DPDU__
			dPdu_sd = mem_alloc(num_global_elements * sizeof(float3));
			dPdu_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float3));
			dPdv_sd = mem_alloc(num_global_elements * sizeof(float3));
			dPdv_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float3));
#endif
			closure_sd = mem_alloc(num_global_elements * ShaderClosure_size);
			closure_sd_DL_shadow = mem_alloc(num_global_elements * 2 * ShaderClosure_size);
			num_closure_sd = mem_alloc(num_global_elements * sizeof(int));
			num_closure_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(int));
			randb_closure_sd = mem_alloc(num_global_elements * sizeof(float));
			randb_closure_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float));
			ray_P_sd = mem_alloc(num_global_elements * sizeof(float3));
			ray_P_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(float3));
			ray_dP_sd = mem_alloc(num_global_elements * sizeof(differential3));
			ray_dP_sd_DL_shadow = mem_alloc(num_global_elements * 2 * sizeof(differential3));

			/* creation of global memory buffers which are shared among the kernels */
			rng_coop = mem_alloc(num_global_elements * rng_size);
			throughput_coop = mem_alloc(num_global_elements * throughput_size);
			L_transparent_coop = mem_alloc(num_global_elements * L_transparent_size);
			PathRadiance_coop = mem_alloc(num_global_elements * PathRadiance_size);
			Ray_coop = mem_alloc(num_global_elements * Ray_size);
			PathState_coop = mem_alloc(num_global_elements * PathState_size);
			Intersection_coop = mem_alloc(num_global_elements * Intersection_size);
			AOAlpha_coop = mem_alloc(num_global_elements * AOAlpha_size);
			AOBSDF_coop = mem_alloc(num_global_elements * AOBSDF_size);
			AOLightRay_coop = mem_alloc(num_global_elements * AOLightRay_size);
			BSDFEval_coop = mem_alloc(num_global_elements * BSDFEval_size);
			ISLamp_coop = mem_alloc(num_global_elements * ISLamp_size);
			LightRay_coop = mem_alloc(num_global_elements * LightRay_size);
			Intersection_coop_AO = mem_alloc(num_global_elements * Intersection_coop_AO_size);
			Intersection_coop_DL = mem_alloc(num_global_elements * Intersection_coop_DL_size);

#ifdef WITH_CYCLES_DEBUG
			debugdata_coop = mem_alloc(num_global_elements * debugdata_size);
#endif

			ray_state = mem_alloc(num_global_elements * rayState_size);

			hostRayStateArray = (char *)calloc(num_global_elements, hostRayState_size);
			assert(hostRayStateArray != NULL && "Can't create hostRayStateArray memory");

			Queue_data = mem_alloc(num_global_elements * (NUM_QUEUES * sizeof(int)+sizeof(int)));
			work_array = mem_alloc(num_global_elements * work_element_size);
			per_sample_output_buffers = mem_alloc(num_global_elements * per_thread_output_buffer_size);
		}

		cl_int dQueue_size = global_size[0] * global_size[1];
		cl_int total_num_rays = global_size[0] * global_size[1];

		/* Set arguments for ckPathTraceKernel_DataInit_SPLIT_KERNEL kernel */
		cl_uint narg = 0;

#define KERNEL_APPEND_ARG(kernel_name, arg) \
		opencl_assert(clSetKernelArg(kernel_name, narg++, sizeof(arg), (void*)&arg))

		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, kgbuffer);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, P_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, P_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, N_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, N_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, Ng_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, Ng_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, I_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, I_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, shader_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, shader_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, flag_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, flag_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, prim_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, prim_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, type_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, type_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, u_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, u_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, v_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, v_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, object_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, object_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, time_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, time_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_length_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_length_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_depth_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_depth_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, transparent_depth_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, transparent_depth_sd_DL_shadow);
#ifdef __RAY_DIFFERENTIALS__
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dP_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dP_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dI_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dI_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, du_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, du_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dv_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dv_sd_DL_shadow);
#endif
#ifdef __DPDU__
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dPdu_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dPdu_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dPdv_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dPdv_sd_DL_shadow);
#endif
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, closure_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, closure_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, num_closure_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, num_closure_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, randb_closure_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, randb_closure_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_P_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_P_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_dP_sd);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_dP_sd_DL_shadow);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_data);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, per_sample_output_buffers);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_rng_state);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, rng_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, throughput_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, L_transparent_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, PathRadiance_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, Ray_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, PathState_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, ray_state);

#define KERNEL_TEX(type, ttype, name) \
	set_kernel_arg_mem(ckPathTraceKernel_DataInit_SPLIT_KERNEL, &narg, #name);
#include "kernel_textures.h"
#undef KERNEL_TEX

		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, start_sample);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_x);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_y);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_w);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_h);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_offset);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, d_stride);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, rtile.rng_state_offset_x);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, rtile.rng_state_offset_y);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, rtile.buffer_rng_state_stride);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, Queue_data);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, Queue_index);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, dQueue_size);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, use_queues_flag);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, work_array);
#ifdef __WORK_STEALING__
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, work_pool_wgs);
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, num_samples);
#endif
#ifdef WITH_CYCLES_DEBUG
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, debugdata_coop);
#endif
		KERNEL_APPEND_ARG(ckPathTraceKernel_DataInit_SPLIT_KERNEL, num_parallel_samples);

		/* Set arguments for ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL */;
		narg = 0;
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, kgbuffer);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, d_data);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, rng_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, Ray_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, PathState_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, Intersection_coop);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, ray_state);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, d_w);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, d_h);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, Queue_data);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, Queue_index);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, dQueue_size);
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, use_queues_flag);
#ifdef WITH_CYCLES_DEBUG
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, debugdata_coop);
#endif
		KERNEL_APPEND_ARG(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, num_parallel_samples);

		/* Set arguments for ckPathTracekernel_LampEmission_SPLIT_KERNEL kernel */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(kgbuffer), (void*)&kgbuffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(sd), (void *)&sd));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(throughput_coop), (void*)&throughput_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(PathRadiance_coop), (void*)&PathRadiance_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(Ray_coop), (void*)&Ray_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(PathState_coop), (void*)&PathState_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(Intersection_coop), (void*)&Intersection_coop));

		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(d_w), (void*)&d_w));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(d_h), (void*)&d_h));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void*)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void*)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void*)&dQueue_size));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(use_queues_flag), (void*)&use_queues_flag));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, narg++, sizeof(num_parallel_samples), (void*)&num_parallel_samples));

		/* Set arguments for ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL kernel */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void*)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void*)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void*)&dQueue_size));

		/* Set arguments for ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL kernel */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(kgbuffer), (void*)&kgbuffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(sd), (void*)&sd));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(per_sample_output_buffers), (void*)&per_sample_output_buffers));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(d_rng_state), (void*)&d_rng_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(rng_coop), (void*)&rng_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(throughput_coop), (void*)&throughput_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(PathRadiance_coop), (void*)&PathRadiance_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(Ray_coop), (void*)&Ray_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(PathState_coop), (void*)&PathState_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(L_transparent_coop), (void*)&L_transparent_coop));

		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(d_w), (void*)&d_w));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(d_h), (void*)&d_h));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(d_x), (void*)&d_x));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(d_y), (void*)&d_y));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(d_stride), (void*)&d_stride));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(rtile.rng_state_offset_x), (void*)&(rtile.rng_state_offset_x)));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(rtile.rng_state_offset_y), (void*)&(rtile.rng_state_offset_y)));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(rtile.buffer_rng_state_stride), (void*)&(rtile.buffer_rng_state_stride)));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(work_array), (void*)&work_array));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void*)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void*)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void*)&dQueue_size));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(end_sample), (void*)&end_sample));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(start_sample), (void*)&start_sample));
#ifdef __WORK_STEALING__
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(work_pool_wgs), (void*)&work_pool_wgs));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(num_samples), (void*)&num_samples));
#endif
#ifdef WITH_CYCLES_DEBUG
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(debugdata_coop), (void*)&debugdata_coop));
#endif
		opencl_assert(clSetKernelArg(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, narg++, sizeof(num_parallel_samples), (void*)&num_parallel_samples));

		/* Set arguments for ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(kgbuffer), (void*)&kgbuffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(sd), (void*)&sd));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(rng_coop), (void*)&rng_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(Ray_coop), (void*)&Ray_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(PathState_coop), (void*)&PathState_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(Intersection_coop), (void*)&Intersection_coop));

		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void *)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void *)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void *)&dQueue_size));

		/* Set up arguments for ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL kernel */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(kgbuffer), (void*)&kgbuffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(sd), (void*)&sd));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(per_sample_output_buffers), (void*)&per_sample_output_buffers));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(rng_coop), (void*)&rng_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(throughput_coop), (void*)&throughput_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(L_transparent_coop), (void*)&L_transparent_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(PathRadiance_coop), (void*)&PathRadiance_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(PathState_coop), (void*)&PathState_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(Intersection_coop), (void*)&Intersection_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(AOAlpha_coop), (void*)&AOAlpha_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(AOBSDF_coop), (void*)&AOBSDF_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(AOLightRay_coop), (void*)&AOLightRay_coop));

		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(d_w), (void*)&d_w));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(d_h), (void*)&d_h));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(d_x), (void*)&d_x));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(d_y), (void*)&d_y));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(d_stride), (void*)&d_stride));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(work_array), (void*)&work_array));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void*)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void*)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void*)&dQueue_size));
#ifdef __WORK_STEALING__
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(start_sample), (void*)&start_sample));
#endif
		opencl_assert(clSetKernelArg(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, narg++, sizeof(num_parallel_samples), (void*)&num_parallel_samples));

		/* Set up arguments for ckPathTraceKernel_DirectLighting_SPLIT_KERNEL kernel */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(kgbuffer), (void*)&kgbuffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(sd), (void*)&sd));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(sd_DL_shadow), (void*)&sd_DL_shadow));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(rng_coop), (void*)&rng_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(PathState_coop), (void*)&PathState_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(ISLamp_coop), (void*)&ISLamp_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(LightRay_coop), (void*)&LightRay_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(BSDFEval_coop), (void*)&BSDFEval_coop));

		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void*)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void*)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void*)&dQueue_size));

		/* Set up arguments for ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL kernel */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(kgbuffer), (void*)&kgbuffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(sd_DL_shadow), (void*)&sd_DL_shadow));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(PathState_coop), (void*)&PathState_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(LightRay_coop), (void*)&LightRay_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(AOLightRay_coop), (void*)&AOLightRay_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(Intersection_coop_AO), (void*)&Intersection_coop_AO));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(Intersection_coop_DL), (void*)&Intersection_coop_DL));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));

		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void*)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void*)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void*)&dQueue_size));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, narg++, sizeof(total_num_rays), (void*)&total_num_rays));

		/* Set up arguments for ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL kernel */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(kgbuffer), (void*)&kgbuffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(d_data), (void*)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(sd), (void*)&sd));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(rng_coop), (void*)&rng_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(throughput_coop), (void*)&throughput_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(PathRadiance_coop), (void*)&PathRadiance_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(Ray_coop), (void*)&Ray_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(PathState_coop), (void*)&PathState_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(LightRay_coop), (void*)&LightRay_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(ISLamp_coop), (void*)&ISLamp_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(BSDFEval_coop), (void*)&BSDFEval_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(AOLightRay_coop), (void*)&AOLightRay_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(AOBSDF_coop), (void*)&AOBSDF_coop));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(AOAlpha_coop), (void*)&AOAlpha_coop));

		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(ray_state), (void*)&ray_state));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(Queue_data), (void*)&Queue_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(Queue_index), (void*)&Queue_index));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(dQueue_size), (void*)&dQueue_size));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, narg++, sizeof(use_queues_flag), (void*)&use_queues_flag));

		/* Set up arguments for ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL */
		narg = 0;
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(d_data), (void *)&d_data));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(d_buffer), (void *)&d_buffer));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(per_sample_output_buffers), (void *)&per_sample_output_buffers));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(num_parallel_samples), (void *)&num_parallel_samples));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(d_w), (void *)&d_w));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(d_h), (void *)&d_h));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(d_stride), (void *)&d_stride));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(rtile.buffer_offset_x), (void *)&(rtile.buffer_offset_x)));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(rtile.buffer_offset_y), (void *)&(rtile.buffer_offset_y)));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(rtile.buffer_rng_state_stride), (void*)&(rtile.buffer_rng_state_stride)));
		opencl_assert(clSetKernelArg(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, narg++, sizeof(start_sample), (void*)&start_sample));


		/* Macro for Enqueuing split kernels */
#define ENQUEUE_SPLIT_KERNEL(kernelName, globalSize, localSize) \
		opencl_assert(clEnqueueNDRangeKernel(cqCommandQueue, kernelName, 2, NULL, globalSize, localSize, 0, NULL, NULL))

		/* Enqueue ckPathTraceKernel_DataInit_SPLIT_KERNEL kernel */
		ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_DataInit_SPLIT_KERNEL, global_size, local_size);
		bool activeRaysAvailable = true;

		/* Record number of time host intervention has been made */
		unsigned int numHostIntervention = 0;
		unsigned int numNextPathIterTimes = PathIteration_times;
		while (activeRaysAvailable) {
			/* Twice the global work size of other kernels for ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL */
			size_t global_size_shadow_blocked[2];
			global_size_shadow_blocked[0] = global_size[0] * 2;
			global_size_shadow_blocked[1] = global_size[1];

			/* Do path-iteration in host [Enqueue Path-iteration kernels] */
			for(int PathIter = 0; PathIter < PathIteration_times; PathIter++) {
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_SceneIntersect_SPLIT_KERNEL, global_size, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_LampEmission_SPLIT_KERNEL, global_size, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_QueueEnqueue_SPLIT_KERNEL, global_size, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_BG_BufferUpdate_SPLIT_KERNEL, global_size, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_Shader_Lighting_SPLIT_KERNEL, global_size, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_Holdout_Emission_Blurring_Pathtermination_AO_SPLIT_KERNEL, global_size, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_DirectLighting_SPLIT_KERNEL, global_size, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_ShadowBlocked_DirectLighting_SPLIT_KERNEL, global_size_shadow_blocked, local_size);
				ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_SetUpNextIteration_SPLIT_KERNEL, global_size, local_size);
			}

			/* Read ray-state into Host memory to decide if we should exit path-iteration in host */
			ciErr = clEnqueueReadBuffer(cqCommandQueue, ray_state, CL_TRUE, 0, global_size[0] * global_size[1] * sizeof(char), hostRayStateArray, 0, NULL, NULL);
			assert(ciErr == CL_SUCCESS);

			activeRaysAvailable = false;

			for(int rayStateIter = 0; rayStateIter < global_size[0] * global_size[1]; rayStateIter++) {
				if(int8_t(hostRayStateArray[rayStateIter]) != RAY_INACTIVE) {
					/* Not all rays are RAY_INACTIVE */
					activeRaysAvailable = true;
					break;
				}
			}

			if(activeRaysAvailable) {
				numHostIntervention++;

				PathIteration_times = PATH_ITER_INC_FACTOR;

				/*
				* Host intervention done before all rays become RAY_INACTIVE;
				* Set do more initial iterations for the next tile
				*/
				numNextPathIterTimes += PATH_ITER_INC_FACTOR;
			}
		}

		/* Execute SumALLRadiance kernel to accumulate radiance calculated in per_sample_output_buffers into RenderTile's output buffer */
		size_t sum_all_radiance_local_size[2] = { 16, 16 };
		size_t sum_all_radiance_global_size[2];
		sum_all_radiance_global_size[0] = (((d_w - 1) / sum_all_radiance_local_size[0]) + 1) * sum_all_radiance_local_size[0];
		sum_all_radiance_global_size[1] = (((d_h - 1) / sum_all_radiance_local_size[1]) + 1) * sum_all_radiance_local_size[1];
		ENQUEUE_SPLIT_KERNEL(ckPathTraceKernel_SumAllRadiance_SPLIT_KERNEL, sum_all_radiance_global_size, sum_all_radiance_local_size);

#undef ENQUEUE_SPLIT_KERNEL

		if(numHostIntervention == 0) {
			/* This means that we are executing kernel more than required
			* Must avoid this for the next sample/tile
			*/
			PathIteration_times = ((numNextPathIterTimes - PATH_ITER_INC_FACTOR) <= 0) ?
			PATH_ITER_INC_FACTOR : numNextPathIterTimes - PATH_ITER_INC_FACTOR;
		}
		else {
			/*
			* Number of path-iterations done for this tile is set as
			* Initial path-iteration times for the next tile
			*/
			PathIteration_times = numNextPathIterTimes;
		}

		first_tile = false;
	}

	/* Calculates the amount of memory that has to be always
	* allocated in order for the split kernel to function.
	* This memory is tile/scene-property invariant (meaning,
	* the value returned by this function does not depend
	* on the user set tile size or scene properties
	*/
	size_t get_invariable_mem_allocated() {
		size_t total_invariable_mem_allocated = 0;
		size_t KernelGlobals_size = 0;
		size_t ShaderData_SOA_size = 0;

		KernelGlobals_size = get_KernelGlobals_size();
		ShaderData_SOA_size = get_shaderdata_soa_size();

		total_invariable_mem_allocated += KernelGlobals_size; /* KernelGlobals size */
		total_invariable_mem_allocated += NUM_QUEUES * sizeof(unsigned int); /* Queue index size */
		total_invariable_mem_allocated += sizeof(char); /* use_queues_flag size */
		total_invariable_mem_allocated += ShaderData_SOA_size; /* sd size */
		total_invariable_mem_allocated += ShaderData_SOA_size; /* sd_DL_shadow size */

		return total_invariable_mem_allocated;
	}

	/* Calculate the memory that has-to-be/has-been allocated for the split kernel to function */
	size_t get_tile_specific_mem_allocated(RenderTile rtile) {
		size_t tile_specific_mem_allocated = 0;

		/* Get required tile info */
		unsigned int user_set_tile_w = rtile.tile_size.x;
		unsigned int user_set_tile_h = rtile.tile_size.y;

#ifdef __WORK_STEALING__
		/* Calculate memory to be allocated for work_pools in case of work_stealing */
		size_t max_global_size[2];
		size_t max_num_work_pools = 0;
		max_global_size[0] = (((user_set_tile_w - 1) / SPLIT_KERNEL_LOCAL_SIZE_X) + 1) * SPLIT_KERNEL_LOCAL_SIZE_X;
		max_global_size[1] = (((user_set_tile_h - 1) / SPLIT_KERNEL_LOCAL_SIZE_Y) + 1) * SPLIT_KERNEL_LOCAL_SIZE_Y;
		max_num_work_pools = (max_global_size[0] * max_global_size[1]) / (SPLIT_KERNEL_LOCAL_SIZE_X * SPLIT_KERNEL_LOCAL_SIZE_Y);
		tile_specific_mem_allocated += max_num_work_pools * sizeof(unsigned int);
#endif

		tile_specific_mem_allocated += user_set_tile_w * user_set_tile_h * per_thread_output_buffer_size;
		tile_specific_mem_allocated += user_set_tile_w * user_set_tile_h * sizeof(RNG);

		return tile_specific_mem_allocated;
	}

	/* Calculates the texture memories and KernelData (d_data) memory that has been allocated */
	size_t get_scene_specific_mem_allocated(cl_mem d_data) {
		size_t scene_specific_mem_allocated = 0;
		/* Calculate texture memories */
#define KERNEL_TEX(type, ttype, name) \
	scene_specific_mem_allocated += get_tex_size(#name);
#include "kernel_textures.h"
#undef KERNEL_TEX

		size_t d_data_size;
		ciErr = clGetMemObjectInfo(d_data, CL_MEM_SIZE, sizeof(d_data_size), &d_data_size, NULL);
		assert(ciErr == CL_SUCCESS && "Can't get d_data mem object info");

		scene_specific_mem_allocated += d_data_size;

		return scene_specific_mem_allocated;
	}

	/* Calculate the memory required for one thread in split kernel */
	size_t get_per_thread_memory() {

		size_t shader_closure_size = 0;
		size_t shaderdata_volume = 0;

#ifdef __MULTI_CLOSURE__
		shader_closure_size = get_shader_closure_size(clos_max);
#else
		shader_closure_size = get_shader_closure_size(MAX_CLOSURE);
#endif
		shaderdata_volume = get_shader_data_size(shader_closure_size);

		size_t retval = rng_size + throughput_size + L_transparent_size + rayState_size + work_element_size
			+ ISLamp_size + PathRadiance_size + Ray_size + PathState_size
			+ Intersection_size                  /* Overall isect */
			+ Intersection_coop_AO_size          /* Instersection_coop_AO */
			+ Intersection_coop_DL_size          /* Intersection coop DL */
			+ shaderdata_volume       /* Overall ShaderData */
			+ (shaderdata_volume * 2) /* ShaderData : DL and shadow */
			+ LightRay_size + BSDFEval_size + AOAlpha_size + AOBSDF_size + AOLightRay_size
			+ (sizeof(int)* NUM_QUEUES)
			+ per_thread_output_buffer_size;

		return retval;
	}

	/* Considers the total memory available in the device and
	* and returns the maximum global work size possible
	*/
	size_t get_feasible_global_work_size(RenderTile rtile, cl_mem d_data) {

		/* Calculate invariably allocated memory */
		size_t invariable_mem_allocated = get_invariable_mem_allocated();
		/* Calculate tile specific allocated memory */
		size_t tile_specific_mem_allocated = get_tile_specific_mem_allocated(rtile);
		/* Calculate scene specific allocated memory */
		size_t scene_specific_mem_allocated = get_scene_specific_mem_allocated(d_data);

		/* Calculate total memory available for the threads in global work size */
		size_t available_memory = total_allocatable_memory
			- invariable_mem_allocated
			- tile_specific_mem_allocated
			- scene_specific_mem_allocated
			- DATA_ALLOCATION_MEM_FACTOR;

		size_t per_thread_memory_required = get_per_thread_memory();

		return (available_memory / per_thread_memory_required);
	}

	/* Checks if the device has enough memory to render the whole tile;
	* If not, we should split single tile into multiple tiles of small size
	* and process them all
	*/
	bool need_to_split_tile(unsigned int d_w, unsigned int d_h, int2 max_render_feasible_tile_size) {
		size_t global_size_estimate[2] = { 0, 0 };
		global_size_estimate[0] = (((d_w - 1) / SPLIT_KERNEL_LOCAL_SIZE_X) + 1) * SPLIT_KERNEL_LOCAL_SIZE_X;
		global_size_estimate[1] = (((d_h - 1) / SPLIT_KERNEL_LOCAL_SIZE_Y) + 1) * SPLIT_KERNEL_LOCAL_SIZE_Y;
		if(global_size_estimate[0] * global_size_estimate[1] > (max_render_feasible_tile_size.x * max_render_feasible_tile_size.y)) {
			return true;
		}
		else {
			return false;
		}
	}

	/* Considers the scene properties, global memory available in the device
	* and returns a rectanglular tile dimension (approx the maximum)
	* that should render on split kernel
	*/
	int2 get_max_render_feasible_tile_size(size_t feasible_global_work_size) {
		int2 max_render_feasible_tile_size;
		int square_root_val = (int)sqrt(feasible_global_work_size);
		max_render_feasible_tile_size.x = square_root_val;
		max_render_feasible_tile_size.y = square_root_val;

		/* ciel round-off max_render_feasible_tile_size */
		int2 ceil_render_feasible_tile_size;
		ceil_render_feasible_tile_size.x = (((max_render_feasible_tile_size.x - 1) / SPLIT_KERNEL_LOCAL_SIZE_X) + 1) * SPLIT_KERNEL_LOCAL_SIZE_X;
		ceil_render_feasible_tile_size.y = (((max_render_feasible_tile_size.y - 1) / SPLIT_KERNEL_LOCAL_SIZE_Y) + 1) * SPLIT_KERNEL_LOCAL_SIZE_Y;

		if(ceil_render_feasible_tile_size.x * ceil_render_feasible_tile_size.y <= feasible_global_work_size) {
			return ceil_render_feasible_tile_size;
		}

		/* floor round-off max_render_feasible_tile_size */
		int2 floor_render_feasible_tile_size;
		floor_render_feasible_tile_size.x = (max_render_feasible_tile_size.x / SPLIT_KERNEL_LOCAL_SIZE_X) * SPLIT_KERNEL_LOCAL_SIZE_X;
		floor_render_feasible_tile_size.y = (max_render_feasible_tile_size.y / SPLIT_KERNEL_LOCAL_SIZE_Y) * SPLIT_KERNEL_LOCAL_SIZE_Y;

		return floor_render_feasible_tile_size;
	}

	/* Try splitting the current tile into multiple smaller almost-square-tiles */
	int2 get_split_tile_size(RenderTile rtile, int2 max_render_feasible_tile_size) {
		int2 split_tile_size;
		int num_global_threads = max_render_feasible_tile_size.x * max_render_feasible_tile_size.y;
		int d_w = rtile.w;
		int d_h = rtile.h;

		/* Ceil round off d_w and d_h */
		d_w = (((d_w - 1) / SPLIT_KERNEL_LOCAL_SIZE_X) + 1) * SPLIT_KERNEL_LOCAL_SIZE_X;
		d_h = (((d_h - 1) / SPLIT_KERNEL_LOCAL_SIZE_Y) + 1) * SPLIT_KERNEL_LOCAL_SIZE_Y;

		while (d_w * d_h > num_global_threads) {
			/* Halve the longer dimension */
			if(d_w >= d_h) {
				d_w = d_w / 2;
				d_w = (((d_w - 1) / SPLIT_KERNEL_LOCAL_SIZE_X) + 1) * SPLIT_KERNEL_LOCAL_SIZE_X;
			}
			else {
				d_h = d_h / 2;
				d_h = (((d_h - 1) / SPLIT_KERNEL_LOCAL_SIZE_Y) + 1) * SPLIT_KERNEL_LOCAL_SIZE_Y;
			}
		}
		split_tile_size.x = d_w;
		split_tile_size.y = d_h;
		return split_tile_size;
	}

	/* Splits existing tile into multiple tiles of tile size split_tile_size */
	vector<RenderTile> split_tiles(RenderTile rtile, int2 split_tile_size) {
		vector<RenderTile> to_path_trace_rtile;

		int d_w = rtile.w;
		int d_h = rtile.h;
		int num_tiles_x = (((d_w - 1) / split_tile_size.x) + 1);
		int num_tiles_y = (((d_h - 1) / split_tile_size.y) + 1);

		/* buffer and rng_state offset calc */
		size_t offset_index = rtile.offset + (rtile.x + rtile.y * rtile.stride);
		size_t offset_x = offset_index % rtile.stride;
		size_t offset_y = offset_index / rtile.stride;

		/* Resize to_path_trace_rtile */
		to_path_trace_rtile.resize(num_tiles_x * num_tiles_y);

		for(int tile_iter_y = 0; tile_iter_y < num_tiles_y; tile_iter_y++) {
			for(int tile_iter_x = 0; tile_iter_x < num_tiles_x; tile_iter_x++) {
				int rtile_index = tile_iter_y * num_tiles_x + tile_iter_x;

				to_path_trace_rtile[rtile_index].rng_state_offset_x = offset_x + tile_iter_x * split_tile_size.x;
				to_path_trace_rtile[rtile_index].rng_state_offset_y = offset_y + tile_iter_y * split_tile_size.y;
				to_path_trace_rtile[rtile_index].buffer_offset_x = offset_x + tile_iter_x * split_tile_size.x;
				to_path_trace_rtile[rtile_index].buffer_offset_y = offset_y + tile_iter_y * split_tile_size.y;
				to_path_trace_rtile[rtile_index].start_sample = rtile.start_sample;
				to_path_trace_rtile[rtile_index].num_samples = rtile.num_samples;
				to_path_trace_rtile[rtile_index].sample = rtile.sample;
				to_path_trace_rtile[rtile_index].resolution = rtile.resolution;
				to_path_trace_rtile[rtile_index].offset = rtile.offset;
				to_path_trace_rtile[rtile_index].tile_size = rtile.tile_size;
				to_path_trace_rtile[rtile_index].buffers = rtile.buffers;
				to_path_trace_rtile[rtile_index].buffer = rtile.buffer;
				to_path_trace_rtile[rtile_index].rng_state = rtile.rng_state;
				to_path_trace_rtile[rtile_index].x = rtile.x + (tile_iter_x * split_tile_size.x);
				to_path_trace_rtile[rtile_index].y = rtile.y + (tile_iter_y * split_tile_size.y);
				to_path_trace_rtile[rtile_index].buffer_rng_state_stride = rtile.stride;

				/* Fill width and height of the new render tile */
				to_path_trace_rtile[rtile_index].w = (tile_iter_x == (num_tiles_x - 1)) ?
					(d_w - (tile_iter_x * split_tile_size.x)) /* Border tile */
					: split_tile_size.x;
				to_path_trace_rtile[rtile_index].h = (tile_iter_y == (num_tiles_y - 1)) ?
					(d_h - (tile_iter_y * split_tile_size.y)) /* Border tile */
					: split_tile_size.y;

				to_path_trace_rtile[rtile_index].stride = to_path_trace_rtile[rtile_index].w;
			}
		}
		return to_path_trace_rtile;
	}

	void thread_run(DeviceTask *task)
	{
		if(task->type == DeviceTask::FILM_CONVERT) {
			film_convert(*task, task->buffer, task->rgba_byte, task->rgba_half);
		}
		else if(task->type == DeviceTask::SHADER) {
			shader(*task);
		}
		else if(task->type == DeviceTask::PATH_TRACE) {
			RenderTile tile;

			bool initialize_data_and_check_render_feasibility = false;
			bool need_to_split_tiles_further = false;
			int2 max_render_feasible_tile_size;
			size_t feasible_global_work_size;

			/* keep rendering tiles until done */
			while (task->acquire_tile(this, tile)) {
				tile.buffer_offset_x = 0;
				tile.buffer_offset_y = 0;
				tile.rng_state_offset_x = 0;
				tile.rng_state_offset_y = 0;

				if(!initialize_data_and_check_render_feasibility) {
					/* Initialize data */
					/* Calculate per_thread_output_buffer_size */
					size_t output_buffer_size = 0;
					ciErr = clGetMemObjectInfo((cl_mem)tile.buffer, CL_MEM_SIZE, sizeof(output_buffer_size), &output_buffer_size, NULL);
					assert(ciErr == CL_SUCCESS && "Can't get tile.buffer mem object info");
					/* This value is different when running on AMD and NV */
					if(background) {
						/* In offline render the number of buffer elements
						* associated with tile.buffer is the current tile size
						*/
						per_thread_output_buffer_size = output_buffer_size / (tile.w * tile.h);
					}
					else {
						/* interactive rendering, unlike offline render, the number of buffer elements
						* associated with tile.buffer is the entire viewport size.
						*/
						per_thread_output_buffer_size = output_buffer_size / (tile.buffers->params.width * tile.buffers->params.height);
					}

					/* Check render feasibility */
					feasible_global_work_size = get_feasible_global_work_size(tile, CL_MEM_PTR(const_mem_map["__data"]->device_pointer));
					max_render_feasible_tile_size = get_max_render_feasible_tile_size(feasible_global_work_size);
					need_to_split_tiles_further = need_to_split_tile(tile.tile_size.x, tile.tile_size.y, max_render_feasible_tile_size);

					initialize_data_and_check_render_feasibility = true;
				}

				if(need_to_split_tiles_further) {

					int2 split_tile_size = get_split_tile_size(tile, max_render_feasible_tile_size);
					vector<RenderTile> to_path_trace_render_tiles = split_tiles(tile, split_tile_size);

					/* Print message to console */
					if(background && (to_path_trace_render_tiles.size() > 1)) {
						fprintf(stderr, "Message : Tiles need to be split further inside path trace (due to insufficient device-global-memory for split kernel to function) \n\
The current tile of dimensions %dx%d is split into tiles of dimension %dx%d for render \n", tile.w, tile.h, split_tile_size.x, split_tile_size.y);
					}

					/* Process all split tiles */
					for(int tile_iter = 0; tile_iter < to_path_trace_render_tiles.size(); tile_iter++) {
						path_trace(to_path_trace_render_tiles[tile_iter], max_render_feasible_tile_size);
					}
				}
				else {
					/* No splitting required; process the entire tile at once */
					/* Render feasible tile size is user-set-tile-size itself */
					max_render_feasible_tile_size.x = (((tile.tile_size.x - 1) / SPLIT_KERNEL_LOCAL_SIZE_X) + 1) * SPLIT_KERNEL_LOCAL_SIZE_X;
					max_render_feasible_tile_size.y = (((tile.tile_size.y - 1) / SPLIT_KERNEL_LOCAL_SIZE_Y) + 1) * SPLIT_KERNEL_LOCAL_SIZE_Y;
					/* buffer_rng_state_stride is stride itself */
					tile.buffer_rng_state_stride = tile.stride;
					path_trace(tile, max_render_feasible_tile_size);
				}
				tile.sample = tile.start_sample + tile.num_samples;

				/* Complete kernel execution before release tile */
				/* This helps in multi-device render;
				 * The device that reaches the critical-section function release_tile
				 * waits (stalling other devices from entering release_tile) for all kernels
				 * to complete. If device1 (a slow-render device) reaches release_tile first then
				 * it would stall device2 (a fast-render device) from proceeding to render next tile
				 */
				clFinish(cqCommandQueue);

				task->release_tile(tile);
			}
		}
	}

	class OpenCLDeviceTask : public DeviceTask {
	public:
		OpenCLDeviceTask(OpenCLDeviceSplitKernel *device, DeviceTask& task)
			: DeviceTask(task)
		{
			run = function_bind(&OpenCLDeviceSplitKernel::thread_run, device, this);
		}
	};

	int get_split_task_count(DeviceTask& /*task*/)
	{
		return 1;
	}

	void task_add(DeviceTask& task)
	{
		task_pool.push(new OpenCLDeviceTask(this, task));
	}

	void task_wait()
	{
		task_pool.wait();
	}

	void task_cancel()
	{
		task_pool.cancel();
	}
};

/* Returns true in case of successful detection of platform and device type,
* else returns false
*/
static bool get_platform_and_devicetype(const DeviceInfo info,
                                        string &platform_name,
                                        cl_device_type &device_type) {
	cl_platform_id platform_id;
	cl_device_id device_id;
	cl_uint num_platforms;
	cl_int ciErr;

	ciErr = clGetPlatformIDs(0, NULL, &num_platforms);
	if(ciErr != CL_SUCCESS) {
		fprintf(stderr, "Can't getPlatformIds. file - %s, line - %d\n", __FILE__, __LINE__);
		return false;
	}

	if(num_platforms == 0) {
		fprintf(stderr, "No OpenCL platforms found. file - %s, line - %d\n", __FILE__, __LINE__);
		return false;
	}

	vector<cl_platform_id> platforms(num_platforms, NULL);

	ciErr = clGetPlatformIDs(num_platforms, &platforms[0], NULL);
	if(ciErr != CL_SUCCESS) {
		fprintf(stderr, "Can't getPlatformIds. file - %s, line - %d\n", __FILE__, __LINE__);
		return false;
	}

	int num_base = 0;
	int total_devices = 0;

	for(int platform = 0; platform < num_platforms; platform++) {
		cl_uint num_devices;

		ciErr = clGetDeviceIDs(platforms[platform], opencl_device_type(), 0, NULL, &num_devices);
		if(ciErr != CL_SUCCESS) {
			fprintf(stderr, "Can't getDeviceIDs. file - %s, line - %d\n", __FILE__, __LINE__);
			return false;
		}

		total_devices += num_devices;

		if(info.num - num_base >= num_devices) {
			/* num doesn't refer to a device in this platform */
			num_base += num_devices;
			continue;
		}

		/* device is in this platform */
		platform_id = platforms[platform];

		/* get devices */
		vector<cl_device_id> device_ids(num_devices, NULL);

		ciErr = clGetDeviceIDs(platform_id, opencl_device_type(), num_devices, &device_ids[0], NULL);
		if(ciErr != CL_SUCCESS) {
			fprintf(stderr, "Can't getDeviceIDs. file - %s, line - %d\n", __FILE__, __LINE__);
			return false;
		}

		device_id = device_ids[info.num - num_base];

		char name[256];
		ciErr = clGetPlatformInfo(platform_id, CL_PLATFORM_NAME, sizeof(name), &name, NULL);
		if(ciErr != CL_SUCCESS) {
			fprintf(stderr, "Can't getPlatformIDs. file - %s, line - %d \n", __FILE__, __LINE__);
			return false;
		}
		platform_name = name;

		ciErr = clGetDeviceInfo(device_id, CL_DEVICE_TYPE, sizeof(cl_device_type), &device_type, NULL);
		if(ciErr != CL_SUCCESS) {
			fprintf(stderr, "Can't getDeviceInfo. file - %s, line - %d \n", __FILE__, __LINE__);
			return false;
		}

		break;
	}

	if(total_devices == 0) {
		fprintf(stderr, "No devices found. file - %s, line - %d \n", __FILE__, __LINE__);
		return false;
	}

	return true;
}

Device *device_opencl_create(DeviceInfo& info, Stats &stats, bool background)
{
	string platform_name;
	cl_device_type device_type;
	if(get_platform_and_devicetype(info, platform_name, device_type)) {
		const bool force_split_kernel = getenv("CYCLES_OPENCL_SPLIT_KERNEL_TEST") != NULL;
		/* TODO(sergey): Replace string lookups with more enum-like API,
		 * similar to device/venfdor checks blender's gpu.
		 */
		if(force_split_kernel ||
		   (platform_name == "AMD Accelerated Parallel Processing" &&
		    device_type == CL_DEVICE_TYPE_GPU))
		{
			/* If the device is an AMD GPU, take split kernel path. */
			VLOG(1) << "Using split kernel";
			return new OpenCLDeviceSplitKernel(info, stats, background);
		} else {
			/* For any other device, take megakernel path. */
			VLOG(1) << "Using megekernel";
			return new OpenCLDeviceMegaKernel(info, stats, background);
		}
	} else {
		/* If we can't retrieve platform and device type information for some reason,
		 * we default to megakernel path.
		 */
		VLOG(1) << "Failed to rertieve platform or device, using megakernel";
		return new OpenCLDeviceMegaKernel(info, stats, background);
	}
}

bool device_opencl_init(void) {
	static bool initialized = false;
	static bool result = false;

	if(initialized)
		return result;

	initialized = true;

	result = clewInit() == CLEW_SUCCESS;

	return result;
}

void device_opencl_info(vector<DeviceInfo>& devices)
{
	vector<cl_device_id> device_ids;
	cl_uint num_devices = 0;
	vector<cl_platform_id> platform_ids;
	cl_uint num_platforms = 0;

	/* get devices */
	if(clGetPlatformIDs(0, NULL, &num_platforms) != CL_SUCCESS || num_platforms == 0)
		return;

	platform_ids.resize(num_platforms);

	if(clGetPlatformIDs(num_platforms, &platform_ids[0], NULL) != CL_SUCCESS)
		return;

	/* devices are numbered consecutively across platforms */
	int num_base = 0;

	for(int platform = 0; platform < num_platforms; platform++, num_base += num_devices) {
		num_devices = 0;
		if(clGetDeviceIDs(platform_ids[platform], opencl_device_type(), 0, NULL, &num_devices) != CL_SUCCESS || num_devices == 0)
			continue;

		device_ids.resize(num_devices);

		if(clGetDeviceIDs(platform_ids[platform], opencl_device_type(), num_devices, &device_ids[0], NULL) != CL_SUCCESS)
			continue;

		char pname[256];
		clGetPlatformInfo(platform_ids[platform], CL_PLATFORM_NAME, sizeof(pname), &pname, NULL);
		string platform_name = pname;

		/* add devices */
		for(int num = 0; num < num_devices; num++) {
			cl_device_id device_id = device_ids[num];
			char name[1024] = "\0";

			if(clGetDeviceInfo(device_id, CL_DEVICE_NAME, sizeof(name), &name, NULL) != CL_SUCCESS)
				continue;

			DeviceInfo info;

			info.type = DEVICE_OPENCL;
			info.description = string(name);
			info.num = num_base + num;
			info.id = string_printf("OPENCL_%d", info.num);
			/* we don't know if it's used for display, but assume it is */
			info.display_device = true;
			info.advanced_shading = opencl_kernel_use_advanced_shading(platform_name);
			info.pack_images = true;

			devices.push_back(info);
		}
	}
}

string device_opencl_capabilities(void)
{
	/* TODO(sergey): Not implemented yet. */
	return "";
}

CCL_NAMESPACE_END

#endif /* WITH_OPENCL */
