#ifndef __MERIDIAN_H__
#define __MERIDIAN_H__

typedef unsigned xbool;
typedef float f32;

typedef signed char      s8;
typedef signed short     s16;
typedef signed int       s32;
typedef signed long long s64;

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

class bin_in;
class prop_array;
class ed_property;

struct xcolor
{
	u8 B, G, R, A;
};

struct vector3
{
	f32 X, Y, Z;
};

struct bbox
{
	vector3 Min;
	vector3 Max;
};

struct guid
{
	u64 Guid;
};

class obj_mgr
{
public:
	guid CreateObject(const char* pObjectTypeName, guid GUID);
};

extern obj_mgr g_ObjMgr;

class object
{
public:
	virtual ~object() = 0;
	virtual void OnInit() = 0;
	virtual void OnKill() = 0;
	virtual void __something1() = 0;
	virtual void OnAdvanceLogic(f32 DeltaTime) = 0;
	virtual void Move(vector3& NewPos, xbool unk) = 0;
	virtual void MoveRel(vector3& DeltaPos, xbool unk) = 0;
	virtual void __something2() = 0;
	virtual void __something3() = 0;
	virtual void __something4() = 0;
	virtual void __something5() = 0;
	virtual void __something6() = 0;
	virtual void __something7() = 0;
	virtual void __something8() = 0;
	virtual void __something9() = 0;
	virtual void __something10() = 0;
	virtual void __something11() = 0;
	virtual void __something12() = 0;
	virtual void __something13() = 0;
	virtual void __something14() = 0;
	virtual void __something15() = 0;
	virtual void __something16() = 0;
	virtual void OnImport(bin_in& BinIn) = 0;
	virtual void __something17() = 0;
	virtual void __something18() = 0;
	virtual void __something19() = 0;
	virtual void __something20() = 0;
	virtual void __something21() = 0;
	virtual void __something22() = 0;
	virtual void __something23() = 0;
	virtual void __something24() = 0;
	virtual void __something25() = 0;
	virtual void __something26() = 0;
	virtual void __something27() = 0;
	virtual void __something28() = 0;
	virtual void __something29() = 0;
	virtual void __something30() = 0;
	virtual void __something31() = 0;
	virtual void __something32() = 0;
	virtual void __something33() = 0;
	virtual void __something34() = 0;
	virtual void __something35() = 0;
	virtual void __something36() = 0;
	virtual void __something37() = 0;
	virtual void EnumerateProperties(prop_array& arr) = 0;
	virtual void SetProperty(const char* pPropName, ed_property& newProp) = 0;
	virtual void GetProperty(ed_property& outProp, const char* pPropName) = 0;

	void SetObjSaveFlag(xbool flag);
};

class marker : public object
{
public:
	void SetText(const char* pNewText);
};

class bin_in
{
public:
	char data[8192];

	bin_in();
	~bin_in();
	xbool OpenFile(const char* pName);

	xbool ReadHeader();
	xbool ReadFields();
};

// xarray
#define STATUS_NORMAL 0
#define STATUS_LOCKED 1
#define STATUS_STATIC 2

// props
enum PROP_Type
{
	PROP_s32 = 0x01,
	PROP_f32 = 0x02,
	PROP_xbool = 0x03,
	PROP_string = 0x04,
	PROP_resource = 0x07,
	PROP_vector3 = 0x08,
	PROP_bbox = 0x09,
	PROP_angle = 0x0a,
	PROP_radian3 = 0x0b,
	PROP_enum = 0x0c,
	PROP_guid = 0x0f,
	PROP_xcolor = 0x11,

	_PROP_forcedword = 0x7fffffff
};

struct ed_property_desc
{
	char m_Name[128];
	PROP_Type m_PropType;
	u32 m_Flags;
	char data[120 + 8];
};

struct ed_property_value
{
	char    m_ResourceName[512];
	bbox    m_BBox;
	xcolor  m_xcolorValue;
	vector3 m_vec3Value;
	u32 unk1[4];
	guid    m_guidValue;
	u32 unk2;
	s32     m_s32Value;
	f32     m_f32Value;
	xbool   m_xboolValue;
};

struct ed_property
{
	PROP_Type m_PropType;
	u32 unk1;
	ed_property_value m_Value;
};

class prop_array // xarray<ed_property_desc>
{
public:

	int _1;
	int _2;
	int m_Count;
	ed_property_desc* pData;
	int m_Capacity;
	int m_Status;
	int _4;

	prop_array()
	{
		_1 = 0;
		_2 = 0;
		m_Count = 0;
		pData = NULL;
		m_Capacity = 0;
		m_Status = STATUS_NORMAL;
		_4 = 0;
	}

	void Clear();
};

#endif
