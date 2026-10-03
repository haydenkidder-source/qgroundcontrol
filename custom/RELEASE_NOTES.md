# Release notes

## v1.11.0

Brings the fork up to upstream QGroundControl master (b837ccd) and keeps every
fork behavior from v1.10.0.

### New from upstream

- **Fly View map position**: the Fly View map position is saved and shared with Plan view.
- **Orbit and loiter radius editing**: edit the radius in both the 2D and 3D map.
- **Log viewer**: ULog array fields can be plotted, and chart interaction is fixed.
- **Spoken status text**: acronyms in vehicle status text are pronounced correctly.
- **Mission commands**: commands that were lost to broken enum translations are restored.
- **MAVLink enums in QML**: enum values are exposed to QML through `MAVLinkEnums`.
- **FTP**: paths are preserved when the `mftp://` scheme is removed.
- **Scripting page**: download and delete icons are centered on the selected script.
- **Terrain in 3D**: terrain cliffs are removed and items sit on the drawn mesh.

### Changes to know about

- **Critical vehicle message popup**: it no longer steals keyboard focus. Escape no
  longer dismisses it. Click outside the popup or use its acknowledge button.
- **FTP directory listing**: the fork keeps its broader fallback to plain listings.
  A generic failure on a timed listing still falls back, as in v1.10.0.
- **Builds**: Android, iOS and macOS builds are no longer produced automatically.
  Linux and Windows builds are unchanged. The three workflows can still be started
  by hand from the Actions tab.

### Behavior kept from v1.10.0

- The Fly View for a vehicle shows only that vehicle's plan. The COP overview can still
  overlay several vehicles' plans.
- Plain-language operator messages and distinct vehicle nicknames.

### Known issues

- Two automated tests still fail on slow CI runners and are tracked for follow-up:
  `Viewer3DUITest` (the fork tool strip has no 3D View button) and
  `RequestMetaDataTypeStateMachineTest` (FTP busy during the parameter download).
  Neither affects flight behavior.
