# Rolling Stock Road Transport

**Version 1.0.0**  
A TesmioLoader plugin for **Workers & Resources: Soviet Republic**.

Rolling Stock Road Transport lets suitable road vehicle carriers transport railway wagons and locomotives. Its primary use is bootstrapping an isolated railway in realistic mode: buy rolling stock at customs, carry it by road, and deliver it into a train depot.

Steam Workshop item: https://steamcommunity.com/sharedfiles/filedetails/?id=3778366463  
Issue / error discussion: https://steamcommunity.com/workshop/filedetails/discussion/3778366463/589560061703165737/  
Repository: https://github.com/Ultimate-Universe/WRSR-TesmioLoader-RollingStockRoadTransport

## Requirements

- Workers & Resources: Soviet Republic
- TesmioLoader
- 64-bit Windows game build
- The current v1.0.0 offsets/signatures were developed and tested against `SOVIET64.exe` version `1.1.1.7`.
- TesmioLoader plugin API version `3`.

Game updates can move native functions or change validation bytes. The plugin fails closed where possible when an expected hook/patch signature is not present.

## Installation

Subscribe to the Steam Workshop item, then copy:

```text
Steam\steamapps\workshop\content\784150\3778366463\plugins\RollingStockRoadTransport.dll
Steam\steamapps\workshop\content\784150\3778366463\plugins\RollingStockRoadTransport.ini
```

to:

```text
Steam\steamapps\common\SovietRepublic\tesmioloader\build\plugins\
```

Start the game through:

```text
Steam\steamapps\common\SovietRepublic\tesmioloader\build\tesmiolauncher.exe
```

Enable `RollingStockRoadTransport.dll` in TesmioLoader.

## How to use

1. Buy or assign any normal road vehicle that is already configured by the game to carry vehicles.
2. Open that truck's **Vehicle type selection** menu.
3. Select a railway wagon or locomotive shown as compatible.
4. Send the truck to the source (for example, a customs house) and load the rolling stock normally.
5. Add a railway depot as the delivery stop.
6. When the truck completes the depot stop, the plugin transfers the carried rolling stock into the depot using the game's native railway-depot storage/insertion path.

## The compatibility rule

The mod deliberately uses the same rule for the selector and for real pickup.

A currently available railway vehicle is compatible when:

```text
total empty mass <= carrier payload limit
AND
carried rail length <= usable longitudinal vehicle-deck length
```

For rolling stock, width and height are not allowed to invalidate a vehicle that passes those two checks.

### Why a smaller truck can show more wagons

Payload is only half of the compatibility test. The game gives vehicle carriers authored vehicle-deck regions, and the mod reads the usable **longitudinal** span of those regions rather than judging the truck by its visual size.

A heavy transporter with a short defined vehicle deck can therefore carry fewer wagon types than a lighter truck with a longer defined deck.

This is expected behaviour. The quickest compatibility check is the truck's **Vehicle type selection** menu.

## Current-year availability

The selector retains the game's rolling-stock availability rules. A wagon/locomotive must be available at the current game date before it is offered for that truck, then it must also pass the truck's payload and deck-length limits.

Workshop rolling stock is evaluated through the same imported-asset list when it uses the normal top-level rail vehicle definitions.

## What the plugin does not do

- It does not turn ordinary cargo trucks into vehicle carriers.
- It does not provide unlimited payload.
- It does not ignore longitudinal deck length.
- It does not permanently change railway depots into road depots.
- It does not use a per-frame building highlighter/world scan to make depot delivery work.
- It does not redistribute game executables, decompiled code, or game assets.

## Configuration

The configuration file is `RollingStockRoadTransport.ini`.

```ini
[RollingStockRoadTransport]
enabled=1
allow_wagons=1
allow_locomotives=1
allow_train_depot_dropoff=1
enable_loading_path_patches=1
enable_actual_load_wagon_scale=1
rail_fit_length_weight_only=1
picker_show_all_fitting_rolling_stock=1
enable_triggered_train_depot_unload=1
triggered_unload_retry_ticks=60
debug_logging=0
debug_trace_limit=20
```

### Setting reference

- `enabled` — master enable switch.
- `allow_wagons` — permits rail wagon type `3`.
- `allow_locomotives` — permits rail locomotive type `4`.
- `allow_train_depot_dropoff` — enables road-carrier compatibility bridging for railway-depot delivery.
- `enable_loading_path_patches` — enables the validated execution gates that let rail candidates reach the native vehicle-load pipeline.
- `enable_actual_load_wagon_scale` — applies the game's carried-vehicle `0.66` longitudinal scale to wagons during actual attachment.
- `rail_fit_length_weight_only` — makes payload + longitudinal deck length authoritative for rail transport eligibility.
- `picker_show_all_fitting_rolling_stock` — rebuilds the route Vehicle type selection rail list using the same weight/length predicate.
- `enable_triggered_train_depot_unload` — transfers carried rail stock through the native train-depot row insertion path when the railway-depot stop completes.
- `triggered_unload_retry_ticks` — retained for backwards-compatible INI parsing; the v1.0.0 delivery path is event-driven and does not use a polling loop.
- `debug_logging` — optional additional diagnostics.
- `debug_trace_limit` — cap for debug traces.

## Technical implementation

### Native target

The plugin is an x64 TesmioLoader DLL. The public build is a conventional Windows PE DLL with a normal `DllMain` entry point and imports only the required Kernel32 functions used for patch protection/cache flushing.

Exported TesmioLoader functions:

- `TsmPluginApiVersion`
- `TsmPluginInit`
- `TsmPluginStart`

### Key game types

```text
VEHICLETYPE_ROAD            = 1
VEHICLETYPE_ROAD_SERVICE    = 2
VEHICLETYPE_RAIL_WAGON      = 3
VEHICLETYPE_RAIL_LOCOMOTIVE = 4
BUILDINGTYPE_ROAD_DEPOT     = 0x0E
BUILDINGTYPE_RAIL_DEPOT     = 0x0F
```

### Unified vehicle-fit hook

- RVA `0x003E2350` — `FUN_1403e2350`

For a normal road vehicle carrier and allowed rail cargo, v1.0.0 uses the shared rail weight/length predicate rather than allowing the generic 3D carried-vehicle packer to make the final eligibility decision.

Weight comes from the rail vehicle's total empty mass, including linked parts where applicable, and is compared with the selected carrier's native vehicle payload capacity (`asset + 0x8604`).

### Rail length

- RVA `0x003E5960` — `FUN_1403e5960`

The game's native total rail-length helper is used so linked rail parts are included. The result is multiplied by the game's existing carried-vehicle scale of `0.66`.

### Carrier deck length

Vehicle deck regions are stored in the carrier asset vector at:

- begin: `asset + 0x7C38`
- end: `asset + 0x7C40`

Each deck region is `0x80` bytes. v1.0.0 reads the longitudinal Z intervals, merges touching/overlapping intervals, and uses the longest continuous usable span. This is why visual truck length and usable vehicle-deck length are not always the same thing.

### Vehicle type selection

- RVA `0x003DE6E0` — `FUN_1403de6e0`
- validated road/rail category-gate patch near RVA `0x003DE86E`

The exact route **Vehicle type selection** popup is hooked. The native list is built, then the rail portion is rebuilt from the game's imported vehicle-asset array. Visible top-level wagons/locomotives are retained only when they are currently available and pass the selected truck's shared payload + longitudinal deck-length rule.

The list therefore functions as the practical compatibility viewer for the selected carrier.

### Actual pickup

- RVA `0x0068A320` — `FUN_14068a320`
- wagon carried-scale parity patch near RVA `0x0068A68C`
- execution gates at RVA `0x006969C6` and `0x006972DC`

The execution gates admit types `3` and `4` into the existing vehicle-loading pipeline. Before native attachment, v1.0.0 verifies the same shared weight/length predicate, including already-carried rolling stock. After that predicate passes, transverse dimensions are temporarily narrowed only for the native placement operation, then restored immediately. Native carried-vector/state updates remain in control.

### Railway-depot route support

- static route patch near RVA `0x002B5806` inside `FUN_1402b50b0`
- building compatibility hook RVA `0x003E2860` — `FUN_1403e2860`

The road route editor is allowed to use railway depots as destinations for eligible vehicle carriers without permanently changing the building type.

### Railway-depot unloading

- RVA `0x0067DA00` — `FUN_14067da00` (native route-stop advance)
- native carried-vehicle unload path RVA `0x0068A010` — `FUN_14068a010`

The working delivery trigger is the native route-stop advance event. Immediately before the game advances away from a completed railway-depot stop, the plugin checks the route-owning vehicle and attached carrier sections for rail cargo. Eligible cargo is passed through the game's native depot-row compatibility/capacity/insertion routines.

This avoids the earlier development approaches that attempted to infer arrival using per-frame route/highlight state.

### Depot storage

The mod does not implement a parallel custom rail-storage format. Railway-depot row capacity and insertion remain native game behaviour. If the depot cannot accept a piece of rolling stock, the plugin does not intentionally bypass that storage limitation.

## Performance design

The release deliberately avoids the expensive experimental approach used during early development.

- No per-frame building-highlight hook.
- No continuous world/building scan.
- No permanent depot-type mutation.
- Selector work happens when the selector is opened.
- Depot transfer happens at the native route-stop advance event.
- Debug traces are capped.
- Static patches validate expected bytes before modification.

## Build

The repository contains a `build.bat` intended for a Visual Studio x64 Native Tools command prompt.

```bat
build.bat
```

It compiles `src/RollingStockRoadTransport.cpp` and produces the DLL and INI in `build\`.

The repository also contains the tiny Kernel32 import objects/library needed by the no-default-runtime build under `tooling\imports\`.

The published binary is built as a standard uncompressed x64 Windows DLL.

## Repository layout

```text
README.md
LICENSE
CHANGELOG.md
build.bat
src/
  RollingStockRoadTransport.cpp
  RollingStockRoadTransport.ini
include/
  tesmio_api.h
tooling/
  imports/
release/
  v1.0.0/
    plugins/
      RollingStockRoadTransport.dll
      RollingStockRoadTransport.ini
    SHA256SUMS.txt
```

## Reporting bugs

Steam discussion:

https://steamcommunity.com/workshop/filedetails/discussion/3778366463/589560061703165737/

GitHub issues:

https://github.com/Ultimate-Universe/WRSR-TesmioLoader-RollingStockRoadTransport/issues

Please include:

- the road carrier used;
- the wagon/locomotive involved;
- source and destination buildings;
- whether the vehicle appears in Vehicle type selection;
- exact reproduction steps;
- `tesmioloader.log`;
- screenshots where useful.

## Licence

Rolling Stock Road Transport is released under the **GNU General Public License v3.0**. See `LICENSE`.

Workers & Resources: Soviet Republic, TesmioLoader, Steam, and their respective names/assets belong to their owners. This project is not affiliated with or endorsed by 3DIVISION, Hooded Horse, Valve, or the TesmioLoader author.
