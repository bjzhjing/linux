/* SPDX-License-Identifier: ((GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Copyright(c) 2016-19 Intel Corporation.
 */
#ifndef _UAPI_ASM_X86_SGX_H
#define _UAPI_ASM_X86_SGX_H

#include <linux/types.h>
#include <linux/ioctl.h>

/**
 * enum sgx_epage_flags - page control flags
 * %SGX_PAGE_MEASURE:	Measure the page contents with a sequence of
 *			ENCLS[EEXTEND] operations.
 */
enum sgx_page_flags {
	SGX_PAGE_MEASURE	= 0x01,
};

#define SGX_MAGIC 0xA4

#define SGX_IOC_ENCLAVE_CREATE \
	_IOW(SGX_MAGIC, 0x00, struct sgx_enclave_create)
#define SGX_IOC_ENCLAVE_ADD_PAGES \
	_IOWR(SGX_MAGIC, 0x01, struct sgx_enclave_add_pages)
#define SGX_IOC_ENCLAVE_INIT \
	_IOW(SGX_MAGIC, 0x02, struct sgx_enclave_init)
#define SGX_IOC_ENCLAVE_PROVISION \
	_IOW(SGX_MAGIC, 0x03, struct sgx_enclave_provision)

/**
 * struct sgx_enclave_create - parameter structure for the
 *                             %SGX_IOC_ENCLAVE_CREATE ioctl
 * @src:	address for the SECS page data
 */
struct sgx_enclave_create  {
	__u64	src;
};

/**
 * struct sgx_enclave_add_pages - parameter structure for the
 *                                %SGX_IOC_ENCLAVE_ADD_PAGE ioctl
 * @src:	start address for the page data
 * @offset:	starting page offset
 * @length:	length of the data (multiple of the page size)
 * @secinfo:	address for the SECINFO data
 * @flags:	page control flags
 * @count:	number of bytes added (multiple of the page size)
 */
struct sgx_enclave_add_pages {
	__u64	src;
	__u64	offset;
	__u64	length;
	__u64	secinfo;
	__u64	flags;
	__u64	count;
};

/**
 * struct sgx_enclave_init - parameter structure for the
 *                           %SGX_IOC_ENCLAVE_INIT ioctl
 * @sigstruct:	address for the SIGSTRUCT data
 */
struct sgx_enclave_init {
	__u64 sigstruct;
};

/**
 * struct sgx_enclave_provision - parameter structure for the
 *				  %SGX_IOC_ENCLAVE_PROVISION ioctl
 * @attribute_fd:	file handle of the attribute file in the securityfs
 */
struct sgx_enclave_provision {
	__u64 attribute_fd;
};

#define SGX_SYNCHRONOUS_EXIT	0
#define SGX_EXCEPTION_EXIT	1

struct sgx_enclave_run;

/**
 * typedef sgx_enclave_exit_handler_t - Exit handler function accepted by
 *					__vdso_sgx_enter_enclave()
 *
 * @rdi:	RDI at the time of enclave exit
 * @rsi:	RSI at the time of enclave exit
 * @rdx:	RDX at the time of enclave exit
 * @ursp:	RSP at the time of enclave exit (untrusted stack)
 * @r8:		R8 at the time of enclave exit
 * @r9:		R9 at the time of enclave exit
 * @r:		Pointer to struct sgx_enclave_run (as provided by caller)
 *
 * Return:
 *  0 or negative to exit vDSO
 *  positive to re-enter enclave (must be EENTER or ERESUME leaf)
 */
typedef int (*sgx_enclave_exit_handler_t)(long rdi, long rsi, long rdx,
					  long ursp, long r8, long r9,
					  struct sgx_enclave_run *r);

/**
 * struct sgx_enclave_exception - structure to report exceptions encountered in
 *				  __vdso_sgx_enter_enclave()
 *
 * @leaf:	ENCLU leaf from \%eax at time of exception
 * @trapnr:	exception trap number, a.k.a. fault vector
 * @error_code:	exception error code
 * @address:	exception address, e.g. CR2 on a #PF
 */
struct sgx_enclave_exception {
	__u32 leaf;
	__u16 trapnr;
	__u16 error_code;
	__u64 address;
};

/**
 * struct sgx_enclave_run - Control structure for __vdso_sgx_enter_enclave()
 *
 * @tcs:		Thread Control Structure used to enter enclave
 * @flags:		Control flags
 * @exit_reason:	Cause of exit from enclave, e.g. EEXIT vs. exception
 * @user_handler:	User provided exit handler (optional)
 * @user_data:		User provided opaque value (optional)
 * @exception:		Valid on exit due to exception
 */
struct sgx_enclave_run {
	__u64 tcs;
	__u32 flags;
	__u32 exit_reason;

	union {
		sgx_enclave_exit_handler_t user_handler;
		__u64 __user_handler;
	};
	__u64 user_data;

	union {
		struct sgx_enclave_exception exception;

		/* Pad the entire struct to 256 bytes. */
		__u8 pad[256 - 32];
	};
};

/**
 * typedef vdso_sgx_enter_enclave_t - Prototype for __vdso_sgx_enter_enclave(),
 *				      a vDSO function to enter an SGX enclave.
 *
 * @rdi:	Pass-through value for RDI
 * @rsi:	Pass-through value for RSI
 * @rdx:	Pass-through value for RDX
 * @leaf:	ENCLU leaf, must be EENTER or ERESUME
 * @r8:		Pass-through value for R8
 * @r9:		Pass-through value for R9
 * @r:		struct sgx_enclave_run, must be non-NULL
 *
 * NOTE: __vdso_sgx_enter_enclave() does not ensure full compliance with the
 * x86-64 ABI, e.g. doesn't handle XSAVE state.  Except for non-volatile
 * general purpose registers, EFLAGS.DF, and RSP alignment, preserving/setting
 * state in accordance with the x86-64 ABI is the responsibility of the enclave
 * and its runtime, i.e. __vdso_sgx_enter_enclave() cannot be called from C
 * code without careful consideration by both the enclave and its runtime.
 *
 * All general purpose registers except RAX, RBX and RCX are passed as-is to
 * the enclave.  RAX, RBX and RCX are consumed by EENTER and ERESUME and are
 * loaded with @leaf, asynchronous exit pointer, and @tcs respectively.
 *
 * RBP and the stack are used to anchor __vdso_sgx_enter_enclave() to the
 * pre-enclave state, e.g. to retrieve @e and @handler after an enclave exit.
 * All other registers are available for use by the enclave and its runtime,
 * e.g. an enclave can push additional data onto the stack (and modify RSP) to
 * pass information to the optional exit handler (see below).
 *
 * Most exceptions reported on ENCLU, including those that occur within the
 * enclave, are fixed up and reported synchronously instead of being delivered
 * via a standard signal. Debug Exceptions (#DB) and Breakpoints (#BP) are
 * never fixed up and are always delivered via standard signals. On synchrously
 * reported exceptions, -EFAULT is returned and details about the exception are
 * recorded in @e, the optional sgx_enclave_exception struct.
 *
 * If an exit handler is provided, the handler will be invoked on synchronous
 * exits from the enclave and for all synchronously reported exceptions. In
 * latter case, @e is filled prior to invoking the handler.
 *
 * The exit handler's return value is interpreted as follows:
 *  >0:		continue, restart __vdso_sgx_enter_enclave() with @ret as @leaf
 *   0:		success, return @ret to the caller
 *  <0:		error, return @ret to the caller
 *
 * The exit handler may transfer control, e.g. via longjmp() or C++ exception,
 * without returning to __vdso_sgx_enter_enclave().
 *
 * Return:
 *  0 on success (ENCLU reached),
 *  -EINVAL if ENCLU leaf is not allowed,
 *  -errno for all other negative values returned by the userspace exit handler
 */
typedef int (*vdso_sgx_enter_enclave_t)(unsigned long rdi, unsigned long rsi,
					unsigned long rdx, unsigned int leaf,
					unsigned long r8,  unsigned long r9,
					struct sgx_enclave_run *r);

#endif /* _UAPI_ASM_X86_SGX_H */
