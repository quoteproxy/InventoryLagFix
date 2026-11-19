# Inventory Lag Fix

An SKSE plugin for Skyrim Special Edition / Anniversary Edition. If you carry a lot of items, opening the
inventory freezes the game for seconds, and the freeze grows quickly with every item you pick up. This
plugin removes that freeze. It also covers the other item screens (containers, shops, gifting, the
favorites menu) and the list refresh after you equip something inside the inventory.

No ESP, no scripts, nothing is written to your save. You can add or remove it at any time.

## Results

Measured on a real save with 1,159 items (4,876 extra-data lists), Skyrim 1.7.104 with SkyUI.
"Frozen" is the time the game spends in frames longer than 0.1 s.

| | Vanilla | With Inventory Lag Fix |
|---|---|---|
| Open the inventory (first time) | 4.1 s | 0.28 s |
| Open the inventory (again) | 4.2 s | 0.26 s |
| Equip an item inside the inventory | 5.5 s | 0.24 s |
| Unequip it again | 1.5 s | under 0.1 s |
| Open the favorites menu | 0.48 s | under 0.1 s |

The same save trimmed to fewer items (time frozen while the inventory opens, average of 2 opens):

| Items | 44 | 110 | 256 | 405 | 605 | 802 | 1,002 | 1,159 |
|---|---|---|---|---|---|---|---|---|
| Vanilla | 0 | 0 | 0.29 s | 0.56 s | 1.19 s | 2.03 s | 3.14 s | 4.16 s |
| With Inventory Lag Fix | 0 | 0 | 0 | 0 | 0 | 0 | 0.17 s | 0.27 s |

The vanilla freeze grows much faster than the item count; with this plugin it stays small.

## Cause

This was found by profiling and reverse engineering `SkyrimSE.exe` 1.7.104. Three places in the game read
the whole inventory again and again, taking a lock for every extra-data list:

1. **Armor perk conditions.** To show an armor rating, `PlayerCharacter::GetArmorValue` (Address Library
   ID 40249) runs every "Mod Armor Rating" perk entry. Conditions such as `WornApparelHasKeywordCount` and
   `WornHasKeyword` call `InventoryChanges::VisitWornItems` (ID 16096), which finds the worn items by
   walking the entire inventory. Matching-set and fitted-armor perks (vanilla, Dawnguard, Dragonborn, the
   Unofficial Patch, Creation Club armor sets) carry dozens of such conditions, so one armor rating costs
   dozens of full inventory scans.
2. **Worn armor per body slot.** For every item card the game collects the armor worn in each of the 32
   body slots (ID 51826) and calls the worn-entry lookup (ID 16091) once per occupied slot. Each lookup
   walks the whole inventory again.
3. **Building the item list.** Menus build their list by asking `InventoryChanges::GetInventoryItemAt`
   (ID 16106) for row 0, 1, 2, ... until it returns nothing, and each call counts from the first item
   again. A list of N rows costs N x N item visits.

## How it is fixed

The game's own code still does all the work. The plugin only skips work that is repeated without
anything changing in between:

1. Inside one `GetArmorValue` call the worn items cannot change. The first `VisitWornItems` call records
   the entries the game visits, and the later calls in the same `GetArmorValue` call replay that list to
   the visitor (same entries, same order, same early-stop rule).
2. Inside one call of 51826, the worn-entry lookup runs on a list made of only the entries that have a
   worn (or worn-left) extra-data list, in their original order. Other entries can never be returned,
   and the only change the lookup can make (un-wearing a leveled item with a negative count) does nothing
   to an entry without a worn list.
3. For the item-list builders of the inventory, container, barter, gift and favorites menus, the next call
   to `GetInventoryItemAt` continues its walk at the item that produced the previous row, with the row
   counter set to that item's first row. Rows from the base container (which come first) are produced by
   the unchanged code; once the position is known, that part is skipped because all its rows lie before
   the requested one. This uses two small jumps inside the function.

Every cache lives for one call (or one list build) on one thread and is dropped afterwards. Each thread
keeps its own. All other calls run the original code unchanged.

## Verified against the original code

`bVerify=1` runs the original code next to every cached call and compares the results (17,743 armor values,
145,750 replayed worn-item lists, 16,608 slot lookups and 6,249 list rows). The release build was checked this way on the save
above: 186,350 comparisons, 0 differences. It was also checked with Inventory Refresh Fix loaded at
the same time.

## Safety

- Nothing is patched unless every Address Library ID resolves and each hooked function (and the two
  jump points inside `GetInventoryItemAt`) matches the expected machine code. Each of the three fixes is
  installed only as a whole.
- If another plugin already patched one of these places, that fix is skipped and the log says so.
- The result is written to `Documents\My Games\Skyrim Special Edition\SKSE\InventoryLagFix.log`.

## Settings

`Data\SKSE\Plugins\InventoryLagFix.ini`:

| key | default | meaning |
|---|---|---|
| `bEnabled` | 1 | 0 = install nothing |
| `bFixArmorPerkConditions` | 1 | fix 1 |
| `bFixWornSlotLookup` | 1 | fix 2 |
| `bFixItemListLookup` | 1 | fix 3 |
| `bLogStats` | 0 | 1 = write cache counters to the log every few seconds |
| `bVerify` | 0 | 1 = testing only: also run the original code and log any difference (much slower) |

## Requirements

- [SKSE64](https://skse.silverlock.org/) for your game version
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (All in one, Anniversary Edition)

## Compatibility

- Built and tested on Skyrim **1.7.104**. On other Anniversary Edition versions (1.6.x) every fix first
  checks the game code and turns itself off if it does not match; check the log to see what was
  installed. Special Edition 1.5.97 is not supported.
- Tested with SkyUI.
- Inventory Refresh Fix and SkyUI Inventory Optimizer are not needed with this plugin. Inventory Refresh
  Fix can stay installed: both were run together and results stayed the same.

## Building

`build.bat` (Visual Studio 2022 or Build Tools 2022, x64). Output: `build\InventoryLagFix.dll`, also copied
to `package\SKSE\Plugins\`.

## License

MIT
