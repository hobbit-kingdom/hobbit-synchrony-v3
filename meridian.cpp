#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <memoryapi.h>
#include <tlhelp32.h>
#include "meridian.hpp"

// engine interface
class _obj_mgr
{
public:
	virtual ~_obj_mgr() = 0;
	int dummy;
};

static inline _obj_mgr* gObjMgr()
{
	return (_obj_mgr*)0x0076CD88;
}


// obj_mgr wrapper implementation
obj_mgr g_ObjMgr;

guid obj_mgr::CreateObject(const char* pObjectTypeName, guid GUID)
{
	guid g;

	typedef u64(__thiscall _obj_mgr::* CreateObjectPROCPTR)(const char* pObjectTypeName, guid GUID);
	CreateObjectPROCPTR pFunc;
	int ptr = 0x0054B7C0;
	memcpy(&pFunc, &ptr, 4);
	g.Guid = (gObjMgr()->*pFunc)(pObjectTypeName, GUID);
	return g;
}

// object
void object::SetObjSaveFlag(xbool flag)
{
	u8* pExtraFlags = ((u8*)this) + 0x7F;
	if (flag)
		*pExtraFlags |= 1;
	else
		*pExtraFlags &= ~1;
}

// marker
void marker::SetText(const char* pNewText)
{
	char* pBuf = ((char*)this) + 0xC0;
	strncpy(pBuf, pNewText, 32);
	pBuf[31] = 0;
}

// bin_in
bin_in::bin_in()
{
	typedef void(__thiscall bin_in::* CtorPROCPTR)();
	CtorPROCPTR pFunc;
	int ptr = 0x0063fb10;
	memcpy(&pFunc, &ptr, 4);
	(this->*pFunc)();
}

bin_in::~bin_in()
{
	typedef void(__thiscall bin_in::* DtorPROCPTR)();
	DtorPROCPTR pFunc;
	int ptr = 0x0063fb50;
	memcpy(&pFunc, &ptr, 4);
	(this->*pFunc)();
}

xbool bin_in::OpenFile(const char* pName)
{
	typedef xbool(__thiscall bin_in::* OpenFilePROCPTR)(const char* pName);
	OpenFilePROCPTR pFunc;
	int ptr = 0x0063fc50;
	memcpy(&pFunc, &ptr, 4);
	return (this->*pFunc)(pName);
}

xbool bin_in::ReadHeader()
{
	typedef xbool(__thiscall bin_in::* ReadHeaderPROCPTR)();
	ReadHeaderPROCPTR pFunc;
	int ptr = 0x0063ff20;
	memcpy(&pFunc, &ptr, 4);
	return (this->*pFunc)();
}

xbool bin_in::ReadFields()
{
	typedef xbool(__thiscall bin_in::* ReadFieldsPROCPTR)();
	ReadFieldsPROCPTR pFunc;
	int ptr = 0x0063ffc0;
	memcpy(&pFunc, &ptr, 4);
	return (this->*pFunc)();
}

// prop_array
void prop_array::Clear()
{
	if (m_Status == 0) {
		typedef void(__cdecl* operator_delete_arr)(void* pArr);
		operator_delete_arr pFunc;
		int ptr = 0x006bf840;
		memcpy(&pFunc, &ptr, 4);
		pFunc(pData);

		pData = NULL;
		m_Capacity = 0;
		m_Count = 0;
	}
	else {
		m_Count = 0;
	}
}