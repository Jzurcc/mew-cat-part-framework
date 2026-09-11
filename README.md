# Mew Item Framework
A DLL dependency mod that allows other mods to add their own custom non-conflicting item sprite frames and inventory icons!

> [!IMPORTANT]
> **Notice:** `MewItemFramework` functionality is officially integrated into [**MewCatPartFramework**](https://www.nexusmods.com/mewgenics/mods/489) (v1.4.0+). It is strongly recommended to use **MewCatPartFramework** instead, as it provides unified support for both custom cat body parts and custom item equipment in a single dependency without conflicts.

> **Current support:** This standalone framework applies to **equippable items and trinkets** (`weapon`, `headItem`, `neckItem`, `faceItem`, `trinket`). For unified cat parts and item support, use [**MewCatPartFramework**](https://www.nexusmods.com/mewgenics/mods/489).

# Making a Custom Item Mod

To make an item mod, install **MewItemFramework**, then create your own mod folder next to it.

First, make sure your `description.json` lists the framework as a requirement so players know to install it:

```json
"requirements": [
    "MewItemFramework>=1.0.0"
]
```

Your mod folder structure should look something like this:

```text
mods/
  MewItemFramework/
    MewItemFramework.dll
  YourItemMod/
    description.json
    preview.png
    item_parts.txt
    swfs/
      your_items.swf
      swflist.gon.append
    data/
      items/
        my_items.gon.append
```

Load your SWF from `swfs/swflist.gon.append`:

```text
game [
    your_items.swf
]
```

## Adding / Registering Items

Custom item frames are registered in `item_parts.txt` using:

```text
id = kind appendBatch logicalPartIndex
```

For example:

```text
myMod.ironSword  = weapon  myMod.weapons  1
myMod.witchHat   = head    myMod.apparel  1
myMod.goldAmulet = neck    myMod.apparel  1
myMod.coolShades = face    myMod.apparel  1
myMod.luckyFish  = trinket myMod.trinkets 1
```

* `id` is the name you will reference from GON files, such as `@myMod.ironSword`
* `kind` is the type of item: `weapon`, `head`, `neck`, `face`, or `trinket`
* `appendBatch` identifies the group of SWF timeline appends this item belongs to
* `logicalPartIndex` is **1-based** and selects the item's frame position inside that appended batch

## SWF ActionScript Linkages

Custom ActionScript linkages identify your movieclips on the FLA/SWF side.
Your SWF ActionScript linkage uses the `__MIF__` marker with the batch ID:

```text
_Append_<Target>__MIF__<appendBatch>
```

The batch name after `__MIF__` must match the batch name used in `item_parts.txt`.

### Companion Targets Per Item Kind

Every custom item requires a complete set of companion MovieClip linkages so that the in-world sprite and all three UI inventory states (**New**, **Worn**, and **Broken**) stay perfectly synchronized on the exact same frame:

| Kind | In-World Sprite Target(s) | UI / Inventory Icon Targets (New, Worn, Broke) | Total Linkages |
| :--- | :--- | :--- | :---: |
| `weapon` | `Weapon` | `WeaponIcon`, `WeaponIcon_Worn`, `WeaponIcon_Broken` | **4** |
| `trinket` | `Trinket` | `TrinketIcon`, `TrinketIcon_Worn`, `TrinketIcon_Broken` | **4** |
| `head` | `HeadItemF`, `HeadItemB` | `HeadItemIcon`, `HeadItemIcon_Worn`, `HeadItemIcon_Broken` | **5** |
| `neck` | `NeckItemF`, `NeckItemB` | `NeckItemIcon`, `NeckItemIcon_Worn`, `NeckItemIcon_Broken` | **5** |
| `face` | `FaceItemF`, `FaceItemB` | `FaceItemIcon`, `FaceItemIcon_Worn`, `FaceItemIcon_Broken` | **5** |

> **Note on `F` and `B` layers:** Head, neck, and face items use two in-world layers: **Front (`F`)** and **Back (`B`)** to correctly render around the cat's head and ears.

### Linkage Examples

For a **weapon** batch named `myMod.weapons` (4 linkages required):

```text
_Append_Weapon__MIF__myMod.weapons
_Append_WeaponIcon__MIF__myMod.weapons
_Append_WeaponIcon_Worn__MIF__myMod.weapons
_Append_WeaponIcon_Broken__MIF__myMod.weapons
```

For a **head item** batch named `myMod.apparel` (5 linkages required):

```text
_Append_HeadItemF__MIF__myMod.apparel
_Append_HeadItemB__MIF__myMod.apparel
_Append_HeadItemIcon__MIF__myMod.apparel
_Append_HeadItemIcon_Worn__MIF__myMod.apparel
_Append_HeadItemIcon_Broken__MIF__myMod.apparel
```

For a **neck item** batch named `myMod.apparel` (5 linkages required):

```text
_Append_NeckItemF__MIF__myMod.apparel
_Append_NeckItemB__MIF__myMod.apparel
_Append_NeckItemIcon__MIF__myMod.apparel
_Append_NeckItemIcon_Worn__MIF__myMod.apparel
_Append_NeckItemIcon_Broken__MIF__myMod.apparel
```

For a **face item** batch named `myMod.apparel` (5 linkages required):

```text
_Append_FaceItemF__MIF__myMod.apparel
_Append_FaceItemB__MIF__myMod.apparel
_Append_FaceItemIcon__MIF__myMod.apparel
_Append_FaceItemIcon_Worn__MIF__myMod.apparel
_Append_FaceItemIcon_Broken__MIF__myMod.apparel
```

For a **trinket** batch named `myMod.trinkets` (4 linkages required):

```text
_Append_Trinket__MIF__myMod.trinkets
_Append_TrinketIcon__MIF__myMod.trinkets
_Append_TrinketIcon_Worn__MIF__myMod.trinkets
_Append_TrinketIcon_Broken__MIF__myMod.trinkets
```

## Using Your Items in GON

Once registered in `item_parts.txt`, reference the item frame ID with `@` inside your item's `frame` property:

```text
IronSword {
    name "Iron Sword"
    kind weapon
    frame @myMod.ironSword
    rarity common
    cost 10
    desc "A sturdy iron blade."
}

WitchHat {
    name "Witch Hat"
    kind head
    frame @myMod.witchHat
    rarity uncommon
    cost 25
    desc "Spooky and stylish."
}

LuckyFish {
    name "Lucky Fish"
    kind trinket
    frame @myMod.luckyFish
    rarity rare
    cost 50
    desc "Smells fishy, but brings good fortune."
}
```

## Automatic Timeline Alignment

MewItemFramework detects and aligns any timeline frame differences across companion targets automatically when custom batches are appended. **Do not add empty padding frames yourself!** Manual padding will interfere with the framework's synchronization.

## Credits & License

* Created by **Jzurcc**
* Derived from [**MewCatPartFramework**](https://github.com/PseudonymTim/MewCatPartFramework) by **Pseudonym_Tim**
* Distributed under the **MIT License**
