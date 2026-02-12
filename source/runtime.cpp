#include "stdafx.h"
#include "ts2fix/runtime.h"

namespace ts2fix
{
RuntimeContext& GetRuntimeContext()
{
	static RuntimeContext runtime = {};
	return runtime;
}
} // namespace ts2fix
