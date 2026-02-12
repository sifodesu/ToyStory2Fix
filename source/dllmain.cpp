#include "stdafx.h"
#include "ts2fix/init.h"

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*lpReserved*/)
{
	if (reason == DLL_PROCESS_ATTACH)
		ts2fix::Init(nullptr);
	return TRUE;
}
