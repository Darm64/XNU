#include <mach/mach_types.h>
#include <chud/chud_xnu.h>

__private_extern__
kern_return_t chudxnu_thread_get_callstack64_kperf(
	thread_t		thread,
	uint64_t		*callstack,
	mach_msg_type_number_t	*count,
	boolean_t		is_user)
{
	return KERN_SUCCESS;
}
