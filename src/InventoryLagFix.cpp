// InventoryLagFix - SKSE plugin for Skyrim Special Edition / Anniversary Edition (1.6.x AE and 1.7.x)
//
// Problem: with a large inventory, opening the inventory (or any item list) freezes the game for
// seconds. Two places in the game look for the player's worn items by walking the WHOLE inventory
// and every extra-data list in it, taking a lock twice per list, and both run many times per item:
//
// 1) Armor perk conditions. For each armor piece, PlayerCharacter::GetArmorValue (Address Library
//    ID 40249) runs every "Mod Armor Rating" perk entry, and conditions such as
//    WornApparelHasKeywordCount / WornHasKeyword call InventoryChanges::VisitWornItems (ID 16096).
//    Matching-set perks (vanilla, Dawnguard, Dragonborn, the Unofficial Patch, Creation Club armor
//    sets) carry dozens of such conditions, so one armor rating costs dozens of full scans.
//    Fix: inside one GetArmorValue call the worn items cannot change, so the first VisitWornItems
//    call records the entries the game visits and later calls in the same GetArmorValue call replay
//    that list to the visitor (same entries, same order, same early-stop rule).
//
// 2) Worn item per body slot. For every item card the game collects the armor currently worn in
//    each of the 32 body slots (ID 51826) and calls the worn-entry lookup (ID 16091) once per
//    occupied slot; each lookup scans the whole inventory again.
//    Fix: inside one call of 51826, the first lookup collects the inventory entries that have at
//    least one worn (or worn-left) extra-data list, and every lookup in that call runs the ORIGINAL
//    lookup code on a list made of only those entries, in their original order. Entries without a
//    worn list can never be returned by the lookup, and the only change the lookup can make to an
//    entry (un-wearing a leveled item with a negative count) does nothing to an entry that has no
//    worn list, so the result and every side effect are the same.
//
// 3) Item lists. Menus build their item list by asking InventoryChanges::GetInventoryItemAt (ID 16106)
//    for row 0, 1, 2, ... until it returns nothing, and every call walks the entry list from the start
//    (normalizing and counting each entry's extra-data lists under locks), so a list of N items costs
//    N x N entry visits. Fix: for the item-list builders of the barter, container, favorites, gift and
//    inventory menus, the next call runs the ORIGINAL function, but its entry-list walk starts at the
//    entry that produced the previous row, with the row counter set to that entry's first row index.
//    The owner's base-container rows (which come first) are produced by the unchanged code; once the
//    walk position is known, later calls skip that part (its rows all lie before the requested one).
//
// Every cache lives for one call of the outer function (or one item-list build) on one thread.
// All other calls run the original code unchanged.
//
// Safety: nothing is changed unless the IDs resolve through Address Library (format 2 for 1.6.x,
// format 5 for 1.7.x) and the start of each hooked function matches the expected machine code.
// Results are written to Documents\My Games\Skyrim Special Edition\SKSE\InventoryLagFix.log

#include <windows.h>
#include <intrin.h>
#include <shlobj.h>
#include <share.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <vector>

#include "AddressLibrary.h"

using UInt32 = std::uint32_t;
using PluginHandle = UInt32;

struct SKSEInterface
{
	UInt32 skseVersion;
	UInt32 runtimeVersion;
	UInt32 editorVersion;
	UInt32 isEditor;
	void* (*QueryInterface)(UInt32 id);
	PluginHandle (*GetPluginHandle)();
};

struct SKSEPluginVersionData
{
	UInt32 dataVersion;
	UInt32 pluginVersion;
	char name[256];
	char author[256];
	char supportEmail[252];
	UInt32 versionIndependenceEx;
	UInt32 versionIndependence;
	UInt32 compatibleVersions[16];
	UInt32 seVersionRequired;
};
static_assert(sizeof(SKSEPluginVersionData) == 848, "SKSEPluginVersionData layout");

extern "C" __declspec(dllexport) SKSEPluginVersionData SKSEPlugin_Version = {
	1,                    // dataVersion (kVersion)
	0x01000000,           // pluginVersion 1.0.0
	"InventoryLagFix",    // name
	"",                   // author
	"",                   // supportEmail
	1,                    // versionIndependenceEx: kVersionIndependentEx_NoStructUse
	1,                    // versionIndependence: kVersionIndependent_AddressLibraryPostAE
	{ 0 },
	0
};

namespace
{
	constexpr const char* kVersionString = "1.0.0";

	enum : std::size_t
	{
		kVisitWornItems,   // InventoryChanges::VisitWornItems(IItemChangeVisitor&)
		kGetArmorValue,    // PlayerCharacter::GetArmorValue(InventoryEntryData*)
		kFindWornEntry,    // InventoryChanges: copy of the worn entry for a body slot
		kCollectWornSlots, // item card: worn armor for each of the 32 body slots
		kIsWorn,           // ExtraDataList::IsWorn(bool either, bool leftOnly)
		kEntryDtor,        // InventoryEntryData::~InventoryEntryData
		kFree,             // game operator delete(void*, size)
		kGetItemAt,        // InventoryChanges::GetInventoryItemAt(index)
		kGetContainer,     // TESObjectREFR::GetContainer
		kListers,          // first of the item-list builders below
		kIdCount = kListers + 7
	};
	// Item-list builders that call GetInventoryItemAt(0), (1), (2) ... until it returns nothing and only
	// read the inventory in between: barter, container, favorites list, gift and inventory menus.
	const std::uint32_t kIds[kIdCount] = { 16096, 40249, 16091, 51826, 11696, 15984, 109588, 16106, 19702,
		50962, 51156, 51544, 51583, 51866, 51867, 51873 };

	constexpr std::uint32_t kContinue = 1;  // BSContainer::ForEachResult::kContinue

	// Expected function starts (1.7.104; checked at runtime on every version).
	// `steal` bytes are whole instructions without RIP-relative operands; they are moved to the trampoline.
	const std::uint8_t kExpectVisitWornItems[] = {
		0x48, 0x89, 0x5C, 0x24, 0x10,  // mov [rsp+10h], rbx
		0x48, 0x89, 0x6C, 0x24, 0x18,  // mov [rsp+18h], rbp
		0x56, 0x41, 0x56, 0x41, 0x57,  // push rsi / push r14 / push r15
		0x48, 0x83, 0xEC, 0x20,        // sub rsp, 20h
		0x48, 0x8B, 0x31               // mov rsi, [rcx]      (entry list)
	};
	constexpr std::size_t kStealVisitWornItems = 5;
	const std::uint8_t kExpectGetArmorValue[] = {
		0x40, 0x57,                                            // push rdi
		0x48, 0x83, 0xEC, 0x50,                                // sub rsp, 50h
		0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,  // mov qword [rsp+20h], -2
		0x48, 0x89, 0x5C, 0x24, 0x68,                          // mov [rsp+68h], rbx
		0x48, 0x8B, 0xFA                                       // mov rdi, rdx        (entry)
	};
	constexpr std::size_t kStealGetArmorValue = 6;
	const std::uint8_t kExpectFindWornEntry[] = {
		0x4C, 0x8B, 0xDC,              // mov r11, rsp
		0x45, 0x88, 0x43, 0x18,        // mov [r11+18h], r8b
		0x55, 0x56, 0x57,              // push rbp / push rsi / push rdi
		0x41, 0x54, 0x41, 0x55,        // push r12 / push r13
		0x41, 0x56, 0x41, 0x57,        // push r14 / push r15
		0x48, 0x83, 0xEC, 0x40         // sub rsp, 40h
	};
	constexpr std::size_t kStealFindWornEntry = 7;
	const std::uint8_t kExpectCollectWornSlots[] = {
		0x48, 0x8B, 0xC4,              // mov rax, rsp
		0x48, 0x89, 0x48, 0x08,        // mov [rax+8], rcx
		0x55, 0x56, 0x57,              // push rbp / push rsi / push rdi
		0x41, 0x54, 0x41, 0x55,        // push r12 / push r13
		0x41, 0x56, 0x41, 0x57,        // push r14 / push r15
		0x48, 0x83, 0xEC, 0x50         // sub rsp, 50h
	};
	constexpr std::size_t kStealCollectWornSlots = 7;
	const std::uint8_t kExpectGetItemAt[] = {
		0x89, 0x54, 0x24, 0x10,        // mov [rsp+10h], edx
		0x48, 0x89, 0x4C, 0x24, 0x08,  // mov [rsp+8], rcx
		0x53, 0x55, 0x56, 0x57,        // push rbx / rbp / rsi / rdi
		0x41, 0x54, 0x41, 0x55,        // push r12 / r13
		0x41, 0x56, 0x41, 0x57         // push r14 / r15
	};
	constexpr std::size_t kStealGetItemAt = 9;
	// Start of the entry-list walk inside GetInventoryItemAt. The walk keeps its row counter at [rsp+0F0h]
	// (read back with the instruction below at least twice). Both are found by their bytes at runtime.
	const std::uint8_t kExpectWalkStart[] = {
		0x48, 0x8B, 0x06,                                // mov rax, [rsi]
		0x48, 0x89, 0x84, 0x24, 0xF8, 0x00, 0x00, 0x00,  // mov [rsp+0F8h], rax
		0x48, 0x85, 0xC0,                                // test rax, rax
		0x0F, 0x84                                       // je ...
	};
	constexpr std::size_t kWalkStartLength = 11;
	const std::uint8_t kExpectCounterUse[] = { 0x8B, 0xB4, 0x24, 0xF0, 0x00, 0x00, 0x00 };  // mov esi, [rsp+0F0h]
	// Start of the base-container loop: "if (baseObjects.size == 0) goto walk start".
	const std::uint8_t kExpectBaseLoop[] = {
		0x83, 0x7C, 0x24, 0x48, 0x00,  // cmp dword [rsp+48h], 0
		0x0F, 0x86                     // jbe rel32 (must point at the walk start)
	};
	constexpr std::size_t kBaseLoopLength = 11;

	using VisitWornItems_t = void (*)(void* changes, void* visitor);
	using GetArmorValue_t = float (*)(void* player, void* entry);
	using FindWornEntry_t = void* (*)(void* changes, std::int32_t slot, bool armorOnly, bool arg4, bool leftOnly);
	using CollectWornSlots_t = void* (*)(void* self);
	using IsWorn_t = bool (*)(void* extraList, bool either, bool leftOnly);
	using EntryDtor_t = void (*)(void* entry);
	using Free_t = void (*)(void* p, std::size_t size);
	using GetItemAt_t = void* (*)(void* changes, std::int32_t index);
	using GetContainer_t = void* (*)(void* ref);
	using VisitFn_t = std::uint32_t (*)(void* visitor, void* entry);

	// BSSimpleList node (the list object is its own head node)
	struct Node
	{
		void* item;
		Node* next;
	};

	FILE* g_log = nullptr;
	std::wstring g_iniPath;
	std::wstring g_pluginDir;

	VisitWornItems_t g_origVisitWornItems = nullptr;
	GetArmorValue_t g_origGetArmorValue = nullptr;
	FindWornEntry_t g_origFindWornEntry = nullptr;
	CollectWornSlots_t g_origCollectWornSlots = nullptr;
	IsWorn_t g_isWorn = nullptr;
	EntryDtor_t g_entryDtor = nullptr;
	Free_t g_free = nullptr;
	GetItemAt_t g_origGetItemAt = nullptr;
	GetContainer_t g_getContainer = nullptr;

	// Return addresses of the GetInventoryItemAt calls inside the item-list builders.
	struct ListSite
	{
		const void* ret;  // address right after the call instruction
		int builder;      // which builder
		bool loop;        // false: the first call (index 0), true: the call inside the loop
	};
	ListSite g_listSites[16];
	int g_listSiteCount = 0;

	DWORD g_mainThread = 0;
	bool g_verify = false;
	bool g_logStats = false;

	// ---- per-thread caches ----
	struct Slot
	{
		void* changes = nullptr;    // InventoryChanges*
		void* entryList = nullptr;  // InventoryChanges::entryList at the time of recording
		std::vector<void*> entries; // InventoryEntryData* passed to Visit(), in order
	};
	constexpr int kSlots = 4;

	struct WornCandidates
	{
		void* changes = nullptr;
		void* entryList = nullptr;
		std::vector<Node> nodes;    // entries that have a worn / worn-left extra-data list, original order
		void* fake[1] = {};         // stand-in InventoryChanges: [0] = entry list
	};

	// Position of the last row an item-list builder received, so the next index continues from there.
	struct ListWalk
	{
		bool active = false;
		int builder = -1;
		void* changes = nullptr;
		void* entryList = nullptr;
		void* owner = nullptr;
		bool resume = false;    // node/start are known: the entry-list walk may start at node
		Node* node = nullptr;   // entry-list node that produced the last row
		std::int32_t start = 0; // index of the first row of that node
		std::int32_t next = 0;  // index the builder is expected to ask for next
		// set only while the hooked call runs
		void* callChanges = nullptr;  // the walk start is replaced only for this object
		bool reachedWalk = false;     // the call got past the base-container rows
	};

	struct ThreadState
	{
		int armorDepth = 0;  // nesting depth of GetArmorValue on this thread
		Slot slots[kSlots];
		std::vector<void*> verifyList;
		int slotDepth = 0;   // nesting depth of CollectWornSlots on this thread
		WornCandidates worn;
		ListWalk walk;
	};
	DWORD g_tls = TLS_OUT_OF_INDEXES;

	ThreadState* State()
	{
		auto* st = static_cast<ThreadState*>(TlsGetValue(g_tls));
		if (!st) {
			// One small block per thread that ever uses a hooked function (kept for the thread's lifetime).
			st = new (std::nothrow) ThreadState();
			if (st) {
				TlsSetValue(g_tls, st);
			}
		}
		return st;
	}

	// ---- counters (only maintained when bLogStats / bVerify is on) ----
	enum : int
	{
		cArmorMain, cArmorOther, cWornVisits, cRecorded, cReplayed, cArmorPassthrough, cListMismatch, cValueMismatch,
		cSlotScopes, cSlotLookups, cSlotBuilds, cSlotCandidates, cSlotPassthrough, cSlotMismatch,
		cItemLists, cItemRows, cItemBaseItems, cItemBroken, cItemLost, cItemMismatch,
		cCount
	};
	const char* const kCounterNames[cCount] = {
		"armorValue(main)", "armorValue(other)", "wornVisits", "recorded", "replayed", "passthrough", "listMismatch", "valueMismatch",
		"slotCards", "slotLookups", "slotBuilds", "slotCandidates", "slotPassthrough", "slotMismatch",
		"itemLists", "itemRows", "itemListsWithBaseItems", "itemListBroken", "itemListLost", "itemRowMismatch"
	};
	volatile LONG64 g_count[cCount] = {};
	LONG64 g_countLogged[cCount] = {};
	volatile LONG64 g_lastStatsLog = 0;

	inline void Count(int c, LONG64 n = 1)
	{
		if (g_logStats) {
			InterlockedAdd64(&g_count[c], n);
		}
	}

	void OpenLog()
	{
		PWSTR docs = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			std::wstring path = docs;
			path += L"\\My Games\\Skyrim Special Edition\\SKSE\\InventoryLagFix.log";
			g_log = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
		}
		if (docs) {
			CoTaskMemFree(docs);
		}
	}

	void Log(const char* fmt, ...)
	{
		if (!g_log) {
			return;
		}
		va_list ap;
		va_start(ap, fmt);
		_lock_file(g_log);
		std::vfprintf(g_log, fmt, ap);
		std::fputc('\n', g_log);
		std::fflush(g_log);
		_unlock_file(g_log);
		va_end(ap);
	}

	void InitPaths()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&InitPaths), &self);
		wchar_t buf[MAX_PATH * 2] = {};
		GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
		std::wstring dll = buf;
		const auto slash = dll.find_last_of(L"\\/");
		g_pluginDir = slash == std::wstring::npos ? L"" : dll.substr(0, slash + 1);
		const auto dot = dll.find_last_of(L'.');
		g_iniPath = (dot == std::wstring::npos ? dll : dll.substr(0, dot)) + L".ini";
	}

	bool IniBool(const wchar_t* section, const wchar_t* key, bool fallback)
	{
		return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, g_iniPath.c_str()) != 0;
	}

	std::uint32_t FormIDOf(void* entry)
	{
		// InventoryEntryData: TESBoundObject* object at +0; TESForm::formID at +0x14
		if (!entry) {
			return 0;
		}
		auto* obj = *static_cast<std::uint8_t**>(entry);
		return obj ? *reinterpret_cast<std::uint32_t*>(obj + 0x14) : 0;
	}

	void MaybeLogStats()
	{
		if (!g_logStats) {
			return;
		}
		const LONG64 now = static_cast<LONG64>(GetTickCount64());
		const LONG64 last = g_lastStatsLog;
		if (now - last < 5000 || InterlockedCompareExchange64(&g_lastStatsLog, now, last) != last) {
			return;
		}
		LONG64 cur[cCount];
		for (int i = 0; i < cCount; ++i) {
			cur[i] = g_count[i];
		}
		if (std::memcmp(cur, g_countLogged, sizeof(cur)) == 0) {
			return;
		}
		std::memcpy(g_countLogged, cur, sizeof(cur));
		char line[1024];
		int len = std::snprintf(line, sizeof(line), "stats:");
		for (int i = 0; i < cCount && len > 0 && len < static_cast<int>(sizeof(line)); ++i) {
			len += std::snprintf(line + len, sizeof(line) - len, " %s=%lld", kCounterNames[i], cur[i]);
		}
		Log("%s", line);
	}

	// ======== fix 1: armor perk conditions ========

	// recording visitor (IItemChangeVisitor layout: vtable with dtor, Visit, ShouldVisit, Unk_03)
	struct Recorder
	{
		void** vtbl;
		std::vector<void*>* out;
	};
	void RecorderDtor(Recorder*, unsigned) {}
	std::uint32_t RecorderVisit(Recorder* self, void* entry)
	{
		self->out->push_back(entry);
		return kContinue;
	}
	bool RecorderShouldVisit(Recorder*, void*, void*) { return true; }
	std::uint32_t RecorderUnk03(Recorder* self, void* entry, void*, bool* arg3)
	{
		if (arg3) {
			*arg3 = true;
		}
		return RecorderVisit(self, entry);
	}
	void* g_recorderVtbl[] = {
		reinterpret_cast<void*>(&RecorderDtor),
		reinterpret_cast<void*>(&RecorderVisit),
		reinterpret_cast<void*>(&RecorderShouldVisit),
		reinterpret_cast<void*>(&RecorderUnk03),
	};

	void Record(void* changes, std::vector<void*>& out)
	{
		out.clear();
		Recorder rec{ g_recorderVtbl, &out };
		g_origVisitWornItems(changes, &rec);
	}

	void ClearSlots(ThreadState* st)
	{
		for (auto& s : st->slots) {
			s.changes = nullptr;
			s.entryList = nullptr;
			s.entries.clear();  // keeps capacity
		}
	}

	void Hook_VisitWornItems(void* changes, void* visitor)
	{
		ThreadState* st = static_cast<ThreadState*>(TlsGetValue(g_tls));
		if (!st || st->armorDepth == 0 || !changes || !visitor) {
			g_origVisitWornItems(changes, visitor);
			return;
		}
		Count(cWornVisits);
		void* entryList = *static_cast<void**>(changes);

		Slot* slot = nullptr;
		for (auto& s : st->slots) {
			if (s.changes == changes && s.entryList == entryList) {
				slot = &s;
				break;
			}
		}
		if (slot) {
			Count(cReplayed);
			if (g_verify) {
				Record(changes, st->verifyList);
				if (st->verifyList != slot->entries) {
					if (InterlockedIncrement64(&g_count[cListMismatch]) <= 20) {
						Log("VERIFY list mismatch: recorded %zu entries, fresh walk %zu", slot->entries.size(), st->verifyList.size());
					}
				}
			}
		} else {
			for (auto& s : st->slots) {
				if (!s.changes) {
					slot = &s;
					break;
				}
			}
			if (!slot) {
				Count(cArmorPassthrough);
				g_origVisitWornItems(changes, visitor);
				return;
			}
			Count(cRecorded);
			slot->changes = changes;
			slot->entryList = entryList;
			Record(changes, slot->entries);
		}

		// Replay exactly like the original loop: stop as soon as Visit() returns anything but kContinue.
		// Index loop: a nested call can only fill other slots, never this vector.
		const auto visit = reinterpret_cast<VisitFn_t>((*static_cast<void***>(visitor))[1]);
		const std::size_t n = slot->entries.size();
		for (std::size_t i = 0; i < n; ++i) {
			if (visit(visitor, slot->entries[i]) != kContinue) {
				break;
			}
		}
	}

	float Hook_GetArmorValue(void* player, void* entry)
	{
		ThreadState* st = State();
		if (!st) {
			return g_origGetArmorValue(player, entry);
		}
		const bool outer = st->armorDepth == 0;
		float reference = 0.0f;
		if (outer) {
			Count(GetCurrentThreadId() == g_mainThread ? cArmorMain : cArmorOther);
			if (g_verify) {
				reference = g_origGetArmorValue(player, entry);  // depth 0: no caching
			}
		}
		++st->armorDepth;
		const float result = g_origGetArmorValue(player, entry);
		if (--st->armorDepth == 0) {
			ClearSlots(st);
		}
		if (outer) {
			if (g_verify && std::memcmp(&reference, &result, sizeof(float)) != 0) {
				if (InterlockedIncrement64(&g_count[cValueMismatch]) <= 20) {
					Log("VERIFY value mismatch: item %08X original %.4f cached %.4f", FormIDOf(entry), reference, result);
				}
			}
			MaybeLogStats();
		}
		return result;
	}

	// ======== fix 2: worn item per body slot ========

	// Same walk and stop rules as the original lookup: stop at the first empty node; per entry, look at
	// extra-data lists up to the first empty node.
	void CollectCandidates(void* changes, WornCandidates& w)
	{
		w.nodes.clear();
		for (auto* node = *static_cast<Node**>(changes); node && node->item; node = node->next) {
			auto* entry = static_cast<std::uint8_t*>(node->item);
			for (auto* ln = *reinterpret_cast<Node**>(entry + 8); ln && ln->item; ln = ln->next) {
				if (g_isWorn(ln->item, true, false)) {
					w.nodes.push_back({ entry, nullptr });
					break;
				}
			}
		}
		if (w.nodes.empty()) {
			w.nodes.push_back({ nullptr, nullptr });  // empty list: the lookup returns nothing
		}
		for (std::size_t i = 0; i + 1 < w.nodes.size(); ++i) {
			w.nodes[i].next = &w.nodes[i + 1];
		}
		w.fake[0] = w.nodes.data();
	}

	bool SameCopy(void* a, void* b)
	{
		// copy layout: object at +0, extra-data list (one node) at +8, count at +0x10
		if (!a || !b) {
			return a == b;
		}
		auto* pa = static_cast<std::uint8_t*>(a);
		auto* pb = static_cast<std::uint8_t*>(b);
		if (*reinterpret_cast<void**>(pa) != *reinterpret_cast<void**>(pb) ||
			*reinterpret_cast<std::int32_t*>(pa + 0x10) != *reinterpret_cast<std::int32_t*>(pb + 0x10)) {
			return false;
		}
		const Node* la = *reinterpret_cast<Node**>(pa + 8);
		const Node* lb = *reinterpret_cast<Node**>(pb + 8);
		return (la ? la->item : nullptr) == (lb ? lb->item : nullptr);
	}

	void* Hook_FindWornEntry(void* changes, std::int32_t slot, bool armorOnly, bool arg4, bool leftOnly)
	{
		ThreadState* st = static_cast<ThreadState*>(TlsGetValue(g_tls));
		if (!st || st->slotDepth == 0 || !changes) {
			return g_origFindWornEntry(changes, slot, armorOnly, arg4, leftOnly);
		}
		Count(cSlotLookups);
		WornCandidates& w = st->worn;
		void* entryList = *static_cast<void**>(changes);
		if (!w.changes) {
			Count(cSlotBuilds);
			w.changes = changes;
			w.entryList = entryList;
			CollectCandidates(changes, w);
			Count(cSlotCandidates, w.nodes.front().item ? static_cast<LONG64>(w.nodes.size()) : 0);
		} else if (w.changes != changes || w.entryList != entryList) {
			Count(cSlotPassthrough);
			return g_origFindWornEntry(changes, slot, armorOnly, arg4, leftOnly);
		}

		void* reference = nullptr;
		if (g_verify) {
			reference = g_origFindWornEntry(changes, slot, armorOnly, arg4, leftOnly);
		}
		void* result = g_origFindWornEntry(w.fake, slot, armorOnly, arg4, leftOnly);
		if (g_verify) {
			if (!SameCopy(reference, result)) {
				if (InterlockedIncrement64(&g_count[cSlotMismatch]) <= 20) {
					Log("VERIFY slot mismatch: slot %d original %08X cached %08X", slot, FormIDOf(reference), FormIDOf(result));
				}
			}
			if (reference) {
				g_entryDtor(reference);
				g_free(reference, 0x18);
			}
		}
		return result;
	}

	void* Hook_CollectWornSlots(void* self)
	{
		ThreadState* st = State();
		if (!st) {
			return g_origCollectWornSlots(self);
		}
		const bool outer = st->slotDepth == 0;
		if (outer) {
			Count(cSlotScopes);
		}
		++st->slotDepth;
		void* result = g_origCollectWornSlots(self);
		if (--st->slotDepth == 0) {
			st->worn.changes = nullptr;
			st->worn.entryList = nullptr;
			st->worn.nodes.clear();  // keeps capacity
			st->worn.fake[0] = nullptr;
		}
		if (outer) {
			MaybeLogStats();
		}
		return result;
	}

	// ======== fix 3: item lists asking for row 0, 1, 2, ... ========

	bool SameRow(void* a, void* b)
	{
		// row copy layout: object at +0, extra-data list at +8, count at +0x10
		if (!a || !b) {
			return a == b;
		}
		auto* pa = static_cast<std::uint8_t*>(a);
		auto* pb = static_cast<std::uint8_t*>(b);
		if (*reinterpret_cast<void**>(pa) != *reinterpret_cast<void**>(pb) ||
			*reinterpret_cast<std::int32_t*>(pa + 0x10) != *reinterpret_cast<std::int32_t*>(pb + 0x10)) {
			return false;
		}
		const Node* la = *reinterpret_cast<Node**>(pa + 8);
		const Node* lb = *reinterpret_cast<Node**>(pb + 8);
		while (la || lb) {
			void* ia = la ? la->item : nullptr;
			void* ib = lb ? lb->item : nullptr;
			if (ia != ib) {
				return false;
			}
			if (!ia) {
				break;
			}
			la = la->next;
			lb = lb->next;
		}
		return true;
	}

	void FreeRow(void* row)
	{
		if (row) {
			g_entryDtor(row);
			g_free(row, 0x18);
		}
	}

	// GetInventoryItemAt(index) first produces the rows of the owner's base-container objects (looking
	// their entries up in the entry list), then walks the entry list from the start, adding each entry's
	// rows to a counter kept at [rsp+0F0h] until the counter reaches `index`. An entry's rows depend only
	// on that entry, so for row i the walk may start at the entry that produced row i-1 with the counter
	// set to that entry's first row index: the result is the same row. Entries before that point were
	// already normalized by the earlier calls of the same list build, so skipping them changes nothing.
	// The base-container part and everything else run unchanged; only where the walk starts differs.
	//
	// Called from a small stub patched in at the start of the walk (rcx = InventoryChanges*,
	// rdx = &counter); returns the node to start from.
	Node* WalkStart(void* changes, std::int32_t* counter)
	{
		auto* st = static_cast<ThreadState*>(TlsGetValue(g_tls));
		if (st && st->walk.callChanges == changes) {
			ListWalk& w = st->walk;
			w.reachedWalk = true;
			if (w.resume) {
				*counter = w.start;
				return w.node;
			}
		}
		return *static_cast<Node**>(changes);
	}

	// Called from a stub at the start of the base-container loop (rcx = InventoryChanges*). Once the walk
	// position is known, every row asked for lies after all base-container rows, and the loop has already
	// run (and normalized those entries) in the earlier calls of this list build, so it can be skipped.
	bool SkipBaseRows(void* changes)
	{
		auto* st = static_cast<ThreadState*>(TlsGetValue(g_tls));
		return st && st->walk.callChanges == changes && st->walk.resume;
	}

	void* Hook_GetItemAt(void* changes, std::int32_t index)
	{
		const void* ret = _ReturnAddress();
		const ListSite* site = nullptr;
		for (int i = 0; i < g_listSiteCount; ++i) {
			if (g_listSites[i].ret == ret) {
				site = &g_listSites[i];
				break;
			}
		}
		ThreadState* st = site && changes ? State() : nullptr;
		if (!st) {
			return g_origGetItemAt(changes, index);
		}
		ListWalk& w = st->walk;
		void** inv = static_cast<void**>(changes);  // [0] entry list, [1] owner reference
		if (!site->loop) {
			w.active = false;
			if (index != 0 || !inv[0]) {
				return g_origGetItemAt(changes, index);
			}
			Count(cItemLists);
			void* container = inv[1] ? g_getContainer(inv[1]) : nullptr;
			if (container && *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(container) + 0x10) != 0) {
				Count(cItemBaseItems);
			}
			w.active = true;
			w.builder = site->builder;
			w.changes = changes;
			w.entryList = inv[0];
			w.owner = inv[1];
			w.resume = false;
			w.node = nullptr;
			w.start = 0;
			w.next = 0;
		} else if (!w.active || w.builder != site->builder || w.changes != changes || w.entryList != inv[0] ||
				   w.owner != inv[1] || index != w.next) {
			if (w.active) {
				Count(cItemBroken);
			}
			w.active = false;
			return g_origGetItemAt(changes, index);
		}

		Count(cItemRows);
		void* reference = g_verify ? g_origGetItemAt(changes, index) : nullptr;  // walk from the start
		w.callChanges = changes;
		w.reachedWalk = false;
		void* row = g_origGetItemAt(changes, index);
		w.callChanges = nullptr;
		if (g_verify) {
			if (!SameRow(reference, row)) {
				if (InterlockedIncrement64(&g_count[cItemMismatch]) <= 20) {
					Log("VERIFY item row mismatch: index %d original %08X cached %08X", index, FormIDOf(reference), FormIDOf(row));
				}
			}
			FreeRow(reference);
		}
		if (!row) {
			w.active = false;  // end of the list
			return row;
		}
		w.next = index + 1;
		if (!w.reachedWalk) {
			return row;  // a base-container row: the walk has not started yet
		}
		void* object = *static_cast<void**>(row);
		Node* n = w.resume ? w.node : static_cast<Node*>(inv[0]);
		while (n && n->item && *static_cast<void**>(n->item) != object) {
			n = n->next;
		}
		if (!n || !n->item) {
			Count(cItemLost);
			w.active = false;
			return row;
		}
		if (!w.resume || n != w.node) {
			w.node = n;
			w.start = index;  // rows are produced in entry order, so this is the entry's first row
			w.resume = true;
		}
		return row;
	}

	// Stub for the start of the entry-list walk inside GetInventoryItemAt, replacing
	//   mov rax, [rsi]            ; rsi = InventoryChanges*, rax = entry list
	//   mov [rsp+0F8h], rax
	// rsp is 16-byte aligned here; all caller-saved registers except rax are preserved.
	std::size_t WriteWalkStub(std::uint8_t* s, std::uintptr_t resume)
	{
		std::size_t i = 0;
		auto put = [&](std::initializer_list<std::uint8_t> b) {
			for (auto x : b) {
				s[i++] = x;
			}
		};
		put({ 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 });  // push rcx, rdx, r8, r9, r10, r11
		put({ 0x48, 0x83, 0xEC, 0x20 });                                      // sub rsp, 20h
		put({ 0x48, 0x89, 0xF1 });                                            // mov rcx, rsi
		put({ 0x48, 0x8D, 0x94, 0x24, 0x40, 0x01, 0x00, 0x00 });              // lea rdx, [rsp+140h] (= old [rsp+0F0h])
		put({ 0x48, 0xB8 });                                                  // mov rax, WalkStart
		const auto fn = reinterpret_cast<std::uintptr_t>(&WalkStart);
		std::memcpy(s + i, &fn, 8);
		i += 8;
		put({ 0xFF, 0xD0 });                                                  // call rax
		put({ 0x48, 0x83, 0xC4, 0x20 });                                      // add rsp, 20h
		put({ 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 });  // pop r11, r10, r9, r8, rdx, rcx
		put({ 0x48, 0x89, 0x84, 0x24, 0xF8, 0x00, 0x00, 0x00 });              // mov [rsp+0F8h], rax
		put({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });                          // jmp [rip+0] -> resume
		std::memcpy(s + i, &resume, 8);
		i += 8;
		return i;
	}

	// Stub for the start of the base-container loop inside GetInventoryItemAt, replacing
	//   cmp dword [rsp+48h], 0
	//   jbe walkStart
	// rax (the loop index) and all caller-saved registers are preserved.
	std::size_t WriteBaseLoopStub(std::uint8_t* s, std::uintptr_t cont, std::uintptr_t walkStart)
	{
		std::size_t i = 0;
		auto put = [&](std::initializer_list<std::uint8_t> b) {
			for (auto x : b) {
				s[i++] = x;
			}
		};
		auto abs = [&](std::uintptr_t a) {
			std::memcpy(s + i, &a, 8);
			i += 8;
		};
		put({ 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 });  // push rax, rcx, rdx, r8-r11
		put({ 0x48, 0x83, 0xEC, 0x28 });                                            // sub rsp, 28h (keeps 16-byte alignment)
		put({ 0x48, 0x89, 0xF1 });                                                  // mov rcx, rsi
		put({ 0x49, 0xBB });                                                        // mov r11, SkipBaseRows
		abs(reinterpret_cast<std::uintptr_t>(&SkipBaseRows));
		put({ 0x41, 0xFF, 0xD3 });                                                  // call r11
		put({ 0x84, 0xC0 });                                                        // test al, al
		put({ 0x48, 0x8D, 0x64, 0x24, 0x28 });                                      // lea rsp, [rsp+28h] (flags kept)
		put({ 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 });  // pop r11-r8, rdx, rcx, rax
		put({ 0x75, 0x15 });                                                        // jnz toWalk
		put({ 0x83, 0x7C, 0x24, 0x48, 0x00 });                                      // cmp dword [rsp+48h], 0
		put({ 0x76, 0x0E });                                                        // jbe toWalk
		put({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });                                // jmp [rip+0] -> loop
		abs(cont);
		put({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });                                // toWalk: jmp [rip+0] -> walk start
		abs(walkStart);
		return i;
	}

	// Replaces `length` bytes at `at` with a jump to `stub` (padded with NOPs).
	bool PatchJump(std::uintptr_t at, std::size_t length, const std::uint8_t* stub)
	{
		const auto rel = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) - static_cast<std::int64_t>(at + 5);
		if (rel < INT32_MIN || rel > INT32_MAX) {
			Log("SKIP: trampoline out of jump range");
			return false;
		}
		DWORD old = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(at), length, PAGE_EXECUTE_READWRITE, &old)) {
			Log("SKIP: VirtualProtect failed (%lu)", GetLastError());
			return false;
		}
		auto* p = reinterpret_cast<std::uint8_t*>(at);
		const auto rel32 = static_cast<std::int32_t>(rel);
		p[0] = 0xE9;
		std::memcpy(p + 1, &rel32, 4);
		for (std::size_t i = 5; i < length; ++i) {
			p[i] = 0x90;
		}
		VirtualProtect(reinterpret_cast<void*>(at), length, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, length);
		return true;
	}

	// Finds the walk start and the base-container loop start inside GetInventoryItemAt by their bytes.
	// Each must occur exactly once, and the loop's jump must lead to the walk start.
	bool FindItemAtPatchPoints(std::uintptr_t fn, std::uintptr_t& walk, std::uintptr_t& loop, std::uintptr_t base)
	{
		DWORD64 imageBase = 0;
		auto* rf = RtlLookupFunctionEntry(fn, &imageBase, nullptr);
		if (!rf || imageBase + rf->BeginAddress != fn) {
			Log("item list fix SKIP: GetInventoryItemAt has no unwind info");
			return false;
		}
		const std::uintptr_t end = imageBase + rf->EndAddress;
		int walks = 0, counters = 0, loops = 0;
		for (std::uintptr_t a = fn; a + sizeof(kExpectWalkStart) <= end; ++a) {
			if (std::memcmp(reinterpret_cast<void*>(a), kExpectWalkStart, sizeof(kExpectWalkStart)) == 0) {
				walk = a;
				++walks;
			}
			if (std::memcmp(reinterpret_cast<void*>(a), kExpectCounterUse, sizeof(kExpectCounterUse)) == 0) {
				++counters;
			}
		}
		if (walks == 1) {
			for (std::uintptr_t a = fn; a + 11 <= end; ++a) {
				if (std::memcmp(reinterpret_cast<void*>(a), kExpectBaseLoop, sizeof(kExpectBaseLoop)) == 0) {
					std::int32_t rel;
					std::memcpy(&rel, reinterpret_cast<void*>(a + 7), 4);
					if (a + 11 + static_cast<std::intptr_t>(rel) == walk) {
						loop = a;
						++loops;
					}
				}
			}
		}
		if (walks != 1 || counters < 2 || loops != 1) {
			Log("item list fix SKIP: unexpected code inside GetInventoryItemAt (walk %d, counter %d, loop %d)", walks, counters, loops);
			return false;
		}
		Log("item list fix: walk start +%llX, base loop +%llX", static_cast<unsigned long long>(walk - base),
			static_cast<unsigned long long>(loop - base));
		return true;
	}

	bool PatchItemAtInternals(std::uintptr_t walk, std::uintptr_t loop, std::uint8_t* stubs)
	{
		WriteWalkStub(stubs, walk + kWalkStartLength);
		WriteBaseLoopStub(stubs + 128, loop + 11, walk);
		FlushInstructionCache(GetCurrentProcess(), stubs, 256);
		// walk start first: the base-loop stub jumps to it
		return PatchJump(walk, kWalkStartLength, stubs) && PatchJump(loop, 11, stubs + 128);
	}

	// Finds the two GetInventoryItemAt calls of one builder: the first (index 0) and the one in its loop.
	bool FindListSites(std::uintptr_t fn, std::uintptr_t target, int builder)
	{
		DWORD64 imageBase = 0;
		auto* rf = RtlLookupFunctionEntry(fn, &imageBase, nullptr);
		if (!rf || imageBase + rf->BeginAddress != fn) {
			return false;
		}
		const std::uintptr_t end = imageBase + rf->EndAddress;
		std::uintptr_t found[3] = {};
		int n = 0;
		for (std::uintptr_t a = fn; a + 5 <= end && n < 3; ++a) {
			const auto* p = reinterpret_cast<const std::uint8_t*>(a);
			if (p[0] != 0xE8) {
				continue;
			}
			std::int32_t rel;
			std::memcpy(&rel, p + 1, 4);
			if (a + 5 + static_cast<std::intptr_t>(rel) == target) {
				found[n++] = a + 5;
			}
		}
		if (n != 2 || g_listSiteCount + 2 > static_cast<int>(std::size(g_listSites))) {
			return false;
		}
		g_listSites[g_listSiteCount++] = { reinterpret_cast<const void*>(found[0]), builder, false };
		g_listSites[g_listSiteCount++] = { reinterpret_cast<const void*>(found[1]), builder, true };
		return true;
	}

	// ======== minimal function-start detour ========
	std::uint8_t* AllocNear(std::uintptr_t target, std::size_t size)
	{
		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		const std::uintptr_t gran = si.dwAllocationGranularity;
		const std::uintptr_t range = 0x7FF00000;
		const std::uintptr_t lo = target > range ? target - range : gran;
		const std::uintptr_t hi = target + range;
		for (std::uintptr_t a = (target & ~(gran - 1)) - gran; a > lo; a -= gran) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
				break;
			}
			if (mbi.State == MEM_FREE) {
				if (auto* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
					return static_cast<std::uint8_t*>(p);
				}
			} else if (reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) < a) {
				a = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) & ~(gran - 1);
			}
		}
		for (std::uintptr_t a = (target + gran) & ~(gran - 1); a < hi;) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
				break;
			}
			if (mbi.State == MEM_FREE) {
				if (auto* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
					return static_cast<std::uint8_t*>(p);
				}
			}
			const std::uintptr_t next = (reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize + gran - 1) & ~(gran - 1);
			a = next > a ? next : a + gran;
		}
		return nullptr;
	}

	void WriteAbsJmp(std::uint8_t* at, std::uintptr_t to)
	{
		at[0] = 0xFF;  // jmp qword ptr [rip+0]
		at[1] = 0x25;
		std::memset(at + 2, 0, 4);
		std::memcpy(at + 6, &to, 8);
	}

	bool Matches(std::uintptr_t fn, const std::uint8_t* expect, std::size_t len, const char* what, std::uintptr_t base)
	{
		if (std::memcmp(reinterpret_cast<void*>(fn), expect, len) == 0) {
			return true;
		}
		char hex[3 * 32 + 1] = {};
		const auto* p = reinterpret_cast<const std::uint8_t*>(fn);
		for (std::size_t i = 0; i < len && i < 32; ++i) {
			std::snprintf(hex + 3 * i, 4, "%02X ", p[i]);
		}
		Log("SKIP: %s at SkyrimSE.exe+%llX has unexpected code (other mod or unsupported version): %s", what,
			static_cast<unsigned long long>(fn - base), hex);
		return false;
	}

	// Stub layout (64 bytes): [0] jmp -> detour, [16] stolen bytes + jmp -> fn+steal (the "original").
	bool Detour(std::uintptr_t fn, std::size_t steal, void* detour, std::uint8_t* stub, void** original)
	{
		const auto rel = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) - static_cast<std::int64_t>(fn + 5);
		if (rel < INT32_MIN || rel > INT32_MAX) {
			Log("SKIP: trampoline out of jump range");
			return false;
		}
		WriteAbsJmp(stub, reinterpret_cast<std::uintptr_t>(detour));
		std::uint8_t* orig = stub + 16;
		std::memcpy(orig, reinterpret_cast<void*>(fn), steal);
		WriteAbsJmp(orig + steal, fn + steal);
		FlushInstructionCache(GetCurrentProcess(), stub, 64);
		*original = orig;

		DWORD old = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(fn), steal, PAGE_EXECUTE_READWRITE, &old)) {
			Log("SKIP: VirtualProtect failed (%lu)", GetLastError());
			return false;
		}
		auto* p = reinterpret_cast<std::uint8_t*>(fn);
		const auto rel32 = static_cast<std::int32_t>(rel);
		p[0] = 0xE9;
		std::memcpy(p + 1, &rel32, 4);
		for (std::size_t i = 5; i < steal; ++i) {
			p[i] = 0x90;
		}
		VirtualProtect(reinterpret_cast<void*>(fn), steal, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, steal);
		return true;
	}
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	OpenLog();
	Log("InventoryLagFix %s", kVersionString);
	if (!skse || skse->isEditor) {
		Log("SKIP: editor or no interface");
		return false;
	}
	InitPaths();
	if (!IniBool(L"General", L"bEnabled", true)) {
		Log("disabled in ini (%ls)", g_iniPath.c_str());
		return true;
	}
	const bool wantArmor = IniBool(L"General", L"bFixArmorPerkConditions", true);
	const bool wantSlots = IniBool(L"General", L"bFixWornSlotLookup", true);
	const bool wantLists = IniBool(L"General", L"bFixItemListLookup", true);
	g_verify = IniBool(L"General", L"bVerify", false);
	g_logStats = IniBool(L"General", L"bLogStats", false) || g_verify;

	const UInt32 v = skse->runtimeVersion;
	wchar_t libName[96];
	std::swprintf(libName, std::size(libName), L"versionlib-%u-%u-%u-%u.bin", v >> 24, (v >> 16) & 0xFF, (v >> 4) & 0xFFF, v & 0xF);
	Log("runtime %u.%u.%u.%u, address library: %ls", v >> 24, (v >> 16) & 0xFF, (v >> 4) & 0xFFF, v & 0xF, libName);

	std::uint64_t off[kIdCount] = {};
	AddressLibrary al;
	if (!al.Resolve(g_pluginDir + libName, kIds, kIdCount, off)) {
		Log("SKIP: %s (format %u). Install Address Library for SKSE Plugins (All in one).", al.error, al.format);
		return true;
	}
	const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
	auto at = [&](std::size_t i) { return base + off[i]; };

	std::uint8_t* stubs = AllocNear(base, 0x1000);
	if (!stubs) {
		Log("SKIP: could not allocate trampoline memory near SkyrimSE.exe");
		return true;
	}
	g_mainThread = GetCurrentThreadId();
	g_tls = TlsAlloc();
	if (g_tls == TLS_OUT_OF_INDEXES) {
		Log("SKIP: TlsAlloc failed");
		return true;
	}

	// Each fix is all-or-nothing; the inner function is hooked first, so its cache can only become
	// active once the outer (scope) function is hooked as well.
	if (!wantArmor) {
		Log("armor perk fix: disabled in ini");
	} else if (Matches(at(kVisitWornItems), kExpectVisitWornItems, sizeof(kExpectVisitWornItems), "VisitWornItems", base) &&
			   Matches(at(kGetArmorValue), kExpectGetArmorValue, sizeof(kExpectGetArmorValue), "GetArmorValue", base) &&
			   Detour(at(kVisitWornItems), kStealVisitWornItems, reinterpret_cast<void*>(&Hook_VisitWornItems), stubs,
				   reinterpret_cast<void**>(&g_origVisitWornItems)) &&
			   Detour(at(kGetArmorValue), kStealGetArmorValue, reinterpret_cast<void*>(&Hook_GetArmorValue), stubs + 64,
				   reinterpret_cast<void**>(&g_origGetArmorValue))) {
		Log("armor perk fix OK: VisitWornItems (+%llX) cached inside GetArmorValue (+%llX)",
			static_cast<unsigned long long>(off[kVisitWornItems]), static_cast<unsigned long long>(off[kGetArmorValue]));
	}

	if (!wantSlots) {
		Log("worn slot fix: disabled in ini");
	} else if (Matches(at(kFindWornEntry), kExpectFindWornEntry, sizeof(kExpectFindWornEntry), "FindWornEntry", base) &&
			   Matches(at(kCollectWornSlots), kExpectCollectWornSlots, sizeof(kExpectCollectWornSlots), "CollectWornSlots", base)) {
		g_isWorn = reinterpret_cast<IsWorn_t>(at(kIsWorn));
		g_entryDtor = reinterpret_cast<EntryDtor_t>(at(kEntryDtor));
		g_free = reinterpret_cast<Free_t>(at(kFree));
		if (Detour(at(kFindWornEntry), kStealFindWornEntry, reinterpret_cast<void*>(&Hook_FindWornEntry), stubs + 128,
				reinterpret_cast<void**>(&g_origFindWornEntry)) &&
			Detour(at(kCollectWornSlots), kStealCollectWornSlots, reinterpret_cast<void*>(&Hook_CollectWornSlots), stubs + 192,
				reinterpret_cast<void**>(&g_origCollectWornSlots))) {
			Log("worn slot fix OK: FindWornEntry (+%llX) limited to worn entries inside CollectWornSlots (+%llX)",
				static_cast<unsigned long long>(off[kFindWornEntry]), static_cast<unsigned long long>(off[kCollectWornSlots]));
		}
	}
	if (!wantLists) {
		Log("item list fix: disabled in ini");
	} else if (std::uintptr_t walk = 0, loop = 0;
			   Matches(at(kGetItemAt), kExpectGetItemAt, sizeof(kExpectGetItemAt), "GetInventoryItemAt", base) &&
			   FindItemAtPatchPoints(at(kGetItemAt), walk, loop, base) && PatchItemAtInternals(walk, loop, stubs + 320)) {
		int builders = 0;
		for (std::size_t i = kListers; i < kIdCount; ++i) {
			if (FindListSites(at(i), at(kGetItemAt), static_cast<int>(i))) {
				++builders;
			} else {
				Log("item list fix: builder id %u skipped (unexpected code)", kIds[i]);
			}
		}
		g_getContainer = reinterpret_cast<GetContainer_t>(at(kGetContainer));
		g_entryDtor = reinterpret_cast<EntryDtor_t>(at(kEntryDtor));
		g_free = reinterpret_cast<Free_t>(at(kFree));
		if (builders > 0 &&
			Detour(at(kGetItemAt), kStealGetItemAt, reinterpret_cast<void*>(&Hook_GetItemAt), stubs + 256,
				reinterpret_cast<void**>(&g_origGetItemAt))) {
			Log("item list fix OK: GetInventoryItemAt (+%llX) continues from the previous row for %d item-list builders",
				static_cast<unsigned long long>(off[kGetItemAt]), builders);
		}
	}
	Log("main thread %lu%s", g_mainThread, g_verify ? ", VERIFY mode (slower: also runs the original code and compares)" : "");
	return true;
}
