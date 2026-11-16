# Error catalog

Section 24 of the shipping plan (docs/ShippingImplementation.md) names the failures a
shipped game must handle the same way every time, each with a stable code. This page maps
every one of them to the engine's own code, where it's defined and the test that proves
the behaviour. The `error_catalog` test (Tests/ErrorCatalogCheck.cmake) reads this table and
fails when:

- a code in section 24 has no row here;
- a row's engine code isn't in its source file, or isn't in the file its test checks it in;
- a row's test isn't a test this build defines, or isn't listed in Tests/RequiredTests.txt
  (so it can't quietly stop running).

The engine's names are the stable ones (`travelErrorName`, `saveErrorName`, diagnostic
codes); a message beside one may change. Where the engine has no single code (an
unsupported cook target, say), the row names the text the tools print and test for.

## Codes

| Code | Engine | Source | Test | Checked in | What happens |
| --- | --- | --- | --- | --- | --- |
| `Busy` | `TravelError::Busy` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `level_travel` | Tests/LevelTravelTest.cpp | a second travel is refused; the active ticket carries on unchanged |
| `UnknownLevel` | `TravelError::UnknownLevel` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `level_travel` | Tests/LevelTravelTest.cpp | refused before a ticket, before anything retires; the message names the level |
| `UnknownSpawn` | `TravelError::UnknownSpawn` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `level_travel` | Tests/LevelTravelTest.cpp | fails before the current level ends, naming the level and the spawn |
| `InvalidCatalog` | `LevelUnreadable` | Source/Runtime/Projects/Private/LevelCatalog.cpp | `level_catalog` | Tests/LevelCatalogTest.cpp | the catalog's diagnostics (`Level*` codes) carry file and line; `player --check --release` fails |
| `MissingAsset` | `AssetMissing` | Source/Runtime/Assets/Private/AssetGraph.cpp | `asset_dependencies` | Tests/AssetDependenciesTest.cpp | the dependency chain that needs it is named; nothing loads a placeholder in its place |
| `ContentMismatch` | `changed since the cook` | Source/Runtime/Packaging/Private/Cook.cpp | `cook_dependencies` | Tests/CookCheck.cmake | `player --cook-info --verify` names each file that no longer matches the cook manifest |
| `BudgetExceeded` | `BudgetExceeded` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `asset_residency` | Tests/AssetResidencyTest.cpp | the level that won't fit is refused with its totals and the budget; the current level plays on |
| `Cancelled` | `TravelError::Cancelled` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `level_travel` | Tests/LevelTravelTest.cpp | ends once; the prepared level is dropped and the old level plays on before the boundary |
| `TooLateToCancel` | `TravelError::TooLateToCancel` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `level_travel` | Tests/LevelTravelTest.cpp | the travel carries on safely; the ticket says it can't be cancelled (the loading screen hides Cancel) |
| `StartFailed` | `TravelError::StartFailed` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `game_flow` | Tests/GameFlowTest.cpp | the candidate is cleaned up; nothing plays and the application can still go to the menu |
| `SaveConflict` | `SaveError::Conflict` | Source/Runtime/GameFramework/Private/SaveGame.cpp | `save_durability` | Tests/SaveCampaignTest.cpp | the stale writer is refused; both revisions are kept, nothing is overwritten silently |
| `IncompatibleSave` | `SaveError::NewerSchema` | Source/Runtime/GameFramework/Private/SaveGame.cpp | `save_migration` | Tests/SaveCampaignTest.cpp | says the save is from a newer version; the file is left as it was |
| `WriteFailed` | `SaveError::WriteFailed` | Source/Runtime/GameFramework/Private/SaveGame.cpp | `save_durability` | Tests/SaveCampaignTest.cpp | the last committed save stays the one Continue uses; no "Saved" message |
| `AuthorityRequired` | `TravelError::AuthorityRequired` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `network_map_rotation` | Tests/NetworkMapRotationTest.cpp | during network play only the host changes maps; a client's request is refused |
| `LoadTimeout` | `TravelError::LoadTimeout` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `level_travel` | Tests/LevelTravelTest.cpp | a level change slower than `transition_timeout_seconds` fails instead of waiting forever (docs/LevelLifetime.md) |
| `LoadTimeout` | `TravelError::LoadTimeout` | Source/Runtime/GameFramework/Private/GameFlow.cpp | `asset_residency` | Tests/AssetResidencyTest.cpp | while assets load on demand the load is stopped, nothing stays loaded, and failure() says why |
| `DeviceUnavailable` | `--inject-device-lost` | Source/Programs/Player/Private/Main.cpp | `failure_recovery` | Tests/RecoveryCheck.cmake | a lost GPU device ends the run with a message, offers safe graphics on the next start and keeps the saves |
| `UnsupportedTarget` | `can't cook for` | Source/Programs/Player/Private/PlayerCook.cpp | `cook_dependencies` | Tests/CookCheck.cmake | a cook or package for a target this engine wasn't built for is refused |
| `IncompleteReleaseEvidence` | `qualified()` | Source/Runtime/Projects/Public/Projects/ReleaseSettings.h | `release` | Tests/ReleaseTest.cpp | a release with any unverified or failed gate isn't qualified (`player --release-info`) |

## Failure injection

Section 25 step 11 asks for injected failures with the last committed save intact. The
`acceptance_failures` test (Tests/AcceptanceCheck.cmake) does it on a copy of Reactor Run
with one level deleted and another corrupted: see docs/Acceptance.md.
