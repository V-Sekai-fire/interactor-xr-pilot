/-
The XR Pilot UI as a transition system: every launch mode, tray entry, window and control is an
action, each recorded as a span named `spanName`. Two checks hold the app to it:

* The tracks hold from any desk: swapping ends on OXRSys with SteamVR's multiple drivers on
  (`swap_ends_on_oxrsys`), picking SteamVR ends on SteamVR (`pick_steamvr_ends_on_steamvr`), unbind
  restores the runtime before the swap (`unbind_restores_the_runtime`), and a round trip
  leaves multiple drivers as the desk had it (`round_trip_restores_multiple_drivers`).
* The tray's three modes end in their rows (`modes_end_in_their_rows`): PCVR on OXRSys, SteamVR on
  OXRSys, and streaming to a headset, where the desk's pilot only watches, read-only
  (`only_the_stream_mode_watches`).
* Every action is reachable: `witness a` is a run from some desk that is valid and ends in `a`
  (`every_action_reachable`, by `decide`).
* A recorded run is a path of the model: `checkSession` replays a session's `ui.*` spans and accepts
  it when some desk makes every span an enabled step.

`emitCpp` writes `core/include/xrpilot/UiSpans.h`, so the app records exactly these names.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Lean.Data.Json
open Lean

namespace XrPilot.Ui

/-- The default OpenXR runtime. -/
inductive Runtime where
  | steamVr | oxrsys | other | none
  deriving DecidableEq, Repr, Inhabited

/-- What a desk is when xr-pilot starts; none of it changes except through an action. -/
structure Desk where
  /-- An MSIX install: the tray offers Uninstall. -/
  packaged : Bool := false
  /-- Another desk copy already holds the tray. -/
  runningCopy : Bool := false
  /-- A first start has already run on this desk (kept in the registry). -/
  firstRunDone : Bool := false
  /-- The default OpenXR runtime, and the one before it (what going back restores). -/
  runtime : Runtime := .steamVr
  previous : Runtime := .none
  /-- SteamVR's `forcedDriver` is `oxrsys`: OXRSys is SteamVR's headset. -/
  forcedOxrsys : Bool := false
  /-- SteamVR's `activateMultipleDrivers`. -/
  multipleDrivers : Bool := false
  /-- What `activateMultipleDrivers` was before the swap to OXRSys turned it on; going back restores it. -/
  multipleDriversBefore : Option Bool := none
  /-- SteamVR's `driver_oxrsys.enable`: whether SteamVR loads OXRSys's driver at all. Once loaded it
  offers a headset and holds the stream's control port, so a desk on SteamVR keeps it off: the Steam
  Frame's Steam Link gets the headset, and a standalone OXRSys runtime gets the port. -/
  oxrsysDriver : Bool := true
  /-- Developer mode is on (kept in the registry). -/
  developerMode : Bool := false
  /-- Windows shows the tray icon; it can fail, as on a shell with no notification area. -/
  trayIcon : Bool := true
  /-- The desk's pilot watches as a read-only spectator (kept in the registry): a headset owns the
  session. -/
  deskWatches : Bool := false
  deriving DecidableEq, Repr, Inhabited

inductive Mode where
  | pilotThenTray | trayOnly | pilotOnly | agent | showRunning | version | usage
  deriving DecidableEq, Repr, Inhabited

/-- The swap track's end: OXRSys is the OpenXR runtime and SteamVR's headset, with SteamVR's other
drivers (trackers, a Quest link) still loading. -/
def Desk.onOxrsys (d : Desk) : Bool := d.runtime == .oxrsys && d.forcedOxrsys && d.multipleDrivers && d.oxrsysDriver

/-- The other track's end: SteamVR is the OpenXR runtime and picks its own headset, with OXRSys's
driver off so it neither offers a headset nor holds the stream's port. -/
def Desk.onSteamVr (d : Desk) : Bool := d.runtime == .steamVr && !d.forcedOxrsys && !d.oxrsysDriver

/-- SteamVR on OXRSys: SteamVR stays the OpenXR runtime, and OXRSys is its headset, streaming to the
desk's pilot or to a headset (the Steam Frame or a Quest 3 running the OXRSys client). -/
def Desk.onSteamVrWithOxrsys (d : Desk) : Bool :=
  d.runtime == .steamVr && d.forcedOxrsys && d.multipleDrivers && d.oxrsysDriver

/-- Swap to OXRSys: the runtime, the forced headset and multiple drivers, remembering what to restore. -/
def Desk.swapToOxrsys (d : Desk) : Desk :=
  { d with runtime := .oxrsys,
           previous := if d.runtime == .oxrsys then d.previous else d.runtime,
           forcedOxrsys := true, multipleDrivers := true, oxrsysDriver := true,
           multipleDriversBefore := some (d.multipleDriversBefore.getD d.multipleDrivers) }

/-- SteamVR on OXRSys: SteamVR is the runtime and OXRSys its headset, with multiple drivers, remembering
what to restore. -/
def Desk.steamVrOnOxrsys (d : Desk) : Desk :=
  { d with runtime := .steamVr,
           previous := if d.runtime == .steamVr then d.previous else d.runtime,
           forcedOxrsys := true, multipleDrivers := true, oxrsysDriver := true,
           multipleDriversBefore := some (d.multipleDriversBefore.getD d.multipleDrivers) }

/-- SteamVR picks its own headset again, multiple drivers is as it was before the swap, and OXRSys's
driver is off. -/
def Desk.handBackHeadset (d : Desk) : Desk :=
  { d with forcedOxrsys := false, multipleDrivers := d.multipleDriversBefore.getD d.multipleDrivers,
           multipleDriversBefore := none, oxrsysDriver := false }

/-- Back to SteamVR from the runtime menu: SteamVR is the runtime and picks its own headset. -/
def Desk.pickSteamVr (d : Desk) : Desk :=
  { d.handBackHeadset with runtime := .steamVr,
                           previous := if d.runtime == .steamVr then d.previous else d.runtime }

/-- Unbind: the runtime before OXRSys comes back (SteamVR when there was none), and SteamVR its headset. -/
def Desk.unbind (d : Desk) : Desk :=
  let back := match d.previous with
    | .steamVr | .other => d.previous
    | .oxrsys | .none => .steamVr
  { d.handBackHeadset with runtime := if d.runtime == .oxrsys then back else d.runtime,
                           previous := if d.runtime == .oxrsys then .none else d.previous }

/-- The swap ends on OXRSys with multiple drivers, from any desk. -/
theorem swap_ends_on_oxrsys (d : Desk) : d.swapToOxrsys.onOxrsys = true := by
  simp [Desk.swapToOxrsys, Desk.onOxrsys]

/-- SteamVR on OXRSys ends there from any desk, and each way back ends on SteamVR with its own headset. -/
theorem steamvr_on_oxrsys_and_back (d : Desk) :
    d.steamVrOnOxrsys.onSteamVrWithOxrsys = true ∧ d.steamVrOnOxrsys.pickSteamVr.onSteamVr = true ∧
    d.steamVrOnOxrsys.unbind.onSteamVr = true := by
  simp [Desk.steamVrOnOxrsys, Desk.onSteamVrWithOxrsys, Desk.pickSteamVr, Desk.unbind, Desk.handBackHeadset,
    Desk.onSteamVr]

/-- Picking SteamVR in the runtime menu ends on SteamVR, from the swap on any desk. -/
theorem pick_steamvr_ends_on_steamvr (d : Desk) : d.swapToOxrsys.pickSteamVr.onSteamVr = true := by
  simp [Desk.swapToOxrsys, Desk.pickSteamVr, Desk.handBackHeadset, Desk.onSteamVr]

/-- Unbind after the swap restores the runtime the desk had (SteamVR when it had none) and hands
SteamVR its headset back; so a desk that came from SteamVR is back on SteamVR. -/
theorem unbind_restores_the_runtime (d : Desk) (h : d.runtime ≠ .oxrsys) :
    d.swapToOxrsys.unbind.runtime = (if d.runtime = .none then .steamVr else d.runtime) ∧
    d.swapToOxrsys.unbind.forcedOxrsys = false := by
  cases hr : d.runtime <;> simp_all [Desk.swapToOxrsys, Desk.unbind, Desk.handBackHeadset]

theorem unbind_from_steamvr_ends_on_steamvr (d : Desk) (h : d.runtime = .steamVr) :
    d.swapToOxrsys.unbind.onSteamVr = true := by
  simp [Desk.swapToOxrsys, Desk.unbind, Desk.handBackHeadset, Desk.onSteamVr, h]

/-- Every way back to SteamVR turns OXRSys's driver off, from any desk, swapped or not: nothing of
OXRSys is left to take the headset from the Steam Frame or the port from a standalone runtime. -/
theorem way_back_turns_the_driver_off (d : Desk) :
    d.pickSteamVr.oxrsysDriver = false ∧ d.unbind.oxrsysDriver = false ∧
    d.handBackHeadset.oxrsysDriver = false := by
  simp [Desk.pickSteamVr, Desk.unbind, Desk.handBackHeadset]

/-- A swap and a way back leave multiple drivers as the desk had it. -/
theorem round_trip_restores_multiple_drivers (d : Desk) (fresh : d.multipleDriversBefore = none) :
    d.swapToOxrsys.unbind.multipleDrivers = d.multipleDrivers ∧
    d.swapToOxrsys.pickSteamVr.multipleDrivers = d.multipleDrivers := by
  simp [Desk.swapToOxrsys, Desk.unbind, Desk.pickSteamVr, Desk.handBackHeadset, fresh]

/-- The change waiting on a background step: an administrator prompt, or the runtime copy before it. -/
inductive Pending where
  | bind | ours | other | unbind
  -- The tray's modes that keep SteamVR the runtime: the desk's pilot owns the session, or watches.
  | steamVrOnOxrsys | stream
  deriving DecidableEq, Repr, Inhabited

inductive Action where
  -- Starting xr-pilot.
  | launchPilot | launchTray | launchWindow | launchAgent | launchShowRunning | launchVersion | launchUsage
  -- The desk copy's set-up when it starts.
  | deskSetup | updateWarning | firstRunBind
  -- The tray menu.
  | trayShowPilot | showRequest | trayBind | trayUnbind | runtimeOurs | runtimeOther
  | modePcvr | modeSteamVr | modeStream
  | addRuntime | addRuntimeChosen | trayUninstall | uninstallConfirm | uninstallCancel
  | openLogs | developerMode | openRuntimeFolder | openDriverFolder | trayQuit
  -- How a background change ends.
  | installFailed | elevatedDone | elevatedDeclined
  -- The pilot window and its controls.
  | windowOpen | windowError | windowClose
  | connect | disconnect | autoConnect | fieldOfView | controllers | pointing | seated
  | appearance | conventions | colourChecker
  deriving DecidableEq, Repr, Inhabited

/-- Every action, once. -/
def Action.all : List Action :=
  [.launchPilot, .launchTray, .launchWindow, .launchAgent, .launchShowRunning, .launchVersion, .launchUsage,
   .deskSetup, .updateWarning, .firstRunBind,
   .trayShowPilot, .showRequest, .trayBind, .trayUnbind, .runtimeOurs, .runtimeOther,
   .modePcvr, .modeSteamVr, .modeStream,
   .addRuntime, .addRuntimeChosen, .trayUninstall, .uninstallConfirm, .uninstallCancel,
   .openLogs, .developerMode, .openRuntimeFolder, .openDriverFolder, .trayQuit,
   .installFailed, .elevatedDone, .elevatedDeclined,
   .windowOpen, .windowError, .windowClose,
   .connect, .disconnect, .autoConnect, .fieldOfView, .controllers, .pointing, .seated,
   .appearance, .conventions, .colourChecker]

/-- The span each action records. -/
def Action.spanName : Action → String
  | .launchPilot => "ui.launch.pilot"
  | .launchTray => "ui.launch.tray"
  | .launchWindow => "ui.launch.window"
  | .launchAgent => "ui.launch.agent"
  | .launchShowRunning => "ui.launch.show-running"
  | .launchVersion => "ui.launch.version"
  | .launchUsage => "ui.launch.usage"
  | .deskSetup => "ui.tray.setup"
  | .updateWarning => "ui.tray.update-warning"
  | .firstRunBind => "ui.tray.first-run-bind"
  | .trayShowPilot => "ui.tray.show-pilot"
  | .showRequest => "ui.tray.show-request"
  | .trayBind => "ui.tray.bind"
  | .trayUnbind => "ui.tray.unbind"
  | .runtimeOurs => "ui.tray.runtime-ours"
  | .runtimeOther => "ui.tray.runtime-other"
  | .modePcvr => "ui.tray.mode-pcvr"
  | .modeSteamVr => "ui.tray.mode-steamvr"
  | .modeStream => "ui.tray.mode-stream"
  | .addRuntime => "ui.tray.add-runtime"
  | .addRuntimeChosen => "ui.tray.add-runtime-chosen"
  | .trayUninstall => "ui.tray.uninstall"
  | .uninstallConfirm => "ui.tray.uninstall-confirm"
  | .uninstallCancel => "ui.tray.uninstall-cancel"
  | .openLogs => "ui.tray.open-logs"
  | .developerMode => "ui.tray.developer-mode"
  | .openRuntimeFolder => "ui.tray.open-runtime-folder"
  | .openDriverFolder => "ui.tray.open-driver-folder"
  | .trayQuit => "ui.tray.quit"
  | .installFailed => "ui.tray.install-failed"
  | .elevatedDone => "ui.tray.elevated-done"
  | .elevatedDeclined => "ui.tray.elevated-declined"
  | .windowOpen => "ui.window.open"
  | .windowError => "ui.window.error"
  | .windowClose => "ui.window.close"
  | .connect => "ui.window.connect"
  | .disconnect => "ui.window.disconnect"
  | .autoConnect => "ui.window.auto-connect"
  | .fieldOfView => "ui.window.field-of-view"
  | .controllers => "ui.window.controllers"
  | .pointing => "ui.window.pointing"
  | .seated => "ui.window.seated"
  | .appearance => "ui.window.appearance"
  | .conventions => "ui.window.conventions"
  | .colourChecker => "ui.window.colour-checker"

/-- The C++ enumerator for each action. -/
def Action.cppName : Action → String
  | .launchPilot => "LaunchPilot"
  | .launchTray => "LaunchTray"
  | .launchWindow => "LaunchWindow"
  | .launchAgent => "LaunchAgent"
  | .launchShowRunning => "LaunchShowRunning"
  | .launchVersion => "LaunchVersion"
  | .launchUsage => "LaunchUsage"
  | .deskSetup => "DeskSetup"
  | .updateWarning => "UpdateWarning"
  | .firstRunBind => "FirstRunBind"
  | .trayShowPilot => "TrayShowPilot"
  | .showRequest => "ShowRequest"
  | .trayBind => "TrayBind"
  | .trayUnbind => "TrayUnbind"
  | .runtimeOurs => "RuntimeOurs"
  | .runtimeOther => "RuntimeOther"
  | .modePcvr => "ModePcvr"
  | .modeSteamVr => "ModeSteamVr"
  | .modeStream => "ModeStream"
  | .addRuntime => "AddRuntime"
  | .addRuntimeChosen => "AddRuntimeChosen"
  | .trayUninstall => "TrayUninstall"
  | .uninstallConfirm => "UninstallConfirm"
  | .uninstallCancel => "UninstallCancel"
  | .openLogs => "OpenLogs"
  | .developerMode => "DeveloperMode"
  | .openRuntimeFolder => "OpenRuntimeFolder"
  | .openDriverFolder => "OpenDriverFolder"
  | .trayQuit => "TrayQuit"
  | .installFailed => "InstallFailed"
  | .elevatedDone => "ElevatedDone"
  | .elevatedDeclined => "ElevatedDeclined"
  | .windowOpen => "WindowOpen"
  | .windowError => "WindowError"
  | .windowClose => "WindowClose"
  | .connect => "Connect"
  | .disconnect => "Disconnect"
  | .autoConnect => "AutoConnect"
  | .fieldOfView => "FieldOfView"
  | .controllers => "Controllers"
  | .pointing => "Pointing"
  | .seated => "Seated"
  | .appearance => "Appearance"
  | .conventions => "Conventions"
  | .colourChecker => "ColourChecker"

def Action.ofSpanName? (name : String) : Option Action :=
  Action.all.find? (·.spanName == name)

structure State where
  desk : Desk
  mode : Option Mode := none
  /-- The desk copy finished its set-up; its tray menu answers. -/
  setUp : Bool := false
  /-- The first start's bind is due once set-up ends. -/
  autoBindDue : Bool := false
  /-- The update warning may follow set-up, before anything else. -/
  warningDue : Bool := false
  window : Bool := false
  /-- The pilot window has been opened (or tried) once. -/
  opened : Bool := false
  /-- A tray click asked for the pilot while its window was closed. -/
  showWanted : Bool := false
  pending : Option Pending := none
  /-- The bind's runtime copy is still ahead of its prompt; a copy that works goes straight on to it. -/
  installing : Bool := false
  uninstallAsked : Bool := false
  fileDialog : Bool := false
  exited : Bool := false
  deriving DecidableEq, Repr, Inhabited

def State.start (d : Desk) : State := { desk := d }

/-- The desk copy: it sets up the desk and owns the tray. -/
def State.ownsDesk (s : State) : Bool := s.mode == some .pilotThenTray || s.mode == some .trayOnly

/-- The tray menu answers: a desk copy, set up, with its icon shown, and no modal dialog open. -/
def State.trayAnswers (s : State) : Bool :=
  s.ownsDesk && s.setUp && s.desk.trayIcon && !s.exited && !s.uninstallAsked

/-- Closing the window quits: a scripted or agent pilot, or a desk copy whose tray icon is missing. -/
def State.quitsOnClose (s : State) : Bool := !s.ownsDesk || !s.desk.trayIcon

/-- When the pilot window may open: a scripted or agent start, a desk copy's first start (after its
set-up, and its first-run bind if due), or a desk copy asked from the tray. -/
def State.canOpen (s : State) : Bool :=
  match s.mode with
  | some .pilotOnly | some .agent => !s.opened
  | some .pilotThenTray => s.setUp && !s.autoBindDue && (!s.opened || s.showWanted)
  -- With no tray icon, nothing could ask for the pilot, so the tray start opens it as a plain start does.
  | some .trayOnly => s.setUp && (s.showWanted || (!s.desk.trayIcon && !s.opened))
  | _ => false

/-- The step an action takes, or `none` where the UI does not offer it. -/
def step (s : State) : Action → Option State
  | .launchPilot =>
    if s.mode.isNone && !s.desk.runningCopy then some { s with mode := some .pilotThenTray } else none
  | .launchTray =>
    if s.mode.isNone && !s.desk.runningCopy then some { s with mode := some .trayOnly } else none
  | .launchWindow => if s.mode.isNone then some { s with mode := some .pilotOnly } else none
  | .launchAgent => if s.mode.isNone then some { s with mode := some .agent } else none
  | .launchShowRunning =>
    if s.mode.isNone && s.desk.runningCopy then some { s with mode := some .showRunning, exited := true } else none
  | .launchVersion => if s.mode.isNone then some { s with mode := some .version, exited := true } else none
  | .launchUsage => if s.mode.isNone then some { s with mode := some .usage, exited := true } else none
  | .deskSetup =>
    if s.ownsDesk && !s.setUp then
      let due := !s.desk.firstRunDone && s.desk.runtime != .oxrsys
      some { s with setUp := true, warningDue := true, autoBindDue := due,
                    desk := { s.desk with firstRunDone := true } }
    else none
  | .updateWarning =>
    if s.setUp && s.warningDue then some { s with warningDue := false } else none
  | .firstRunBind =>
    if s.setUp && s.autoBindDue && s.pending.isNone then
      some { s with autoBindDue := false, warningDue := false, pending := some .bind, installing := true }
    else none
  | .trayShowPilot =>
    if s.trayAnswers then
      some { s with warningDue := false, showWanted := !s.window } else none
  | .showRequest =>
    if s.trayAnswers then some { s with warningDue := false, showWanted := !s.window } else none
  | .trayBind =>
    if s.trayAnswers then
      -- A click while a change is pending does nothing.
      if s.pending.isNone then some { s with warningDue := false, pending := some .bind, installing := true }
      else some { s with warningDue := false }
    else none
  | .trayUnbind =>
    if s.trayAnswers then
      if s.pending.isNone then some { s with warningDue := false, pending := some .unbind } else some { s with warningDue := false }
    else none
  | .runtimeOurs =>
    if s.trayAnswers then
      if s.pending.isNone then some { s with warningDue := false, pending := some .ours } else some { s with warningDue := false }
    else none
  | .runtimeOther =>
    if s.trayAnswers then
      if s.pending.isNone then some { s with warningDue := false, pending := some .other } else some { s with warningDue := false }
    else none
  -- Each mode installs OXRSys first, as Bind does. PCVR on OXRSys is the swap; the other two keep SteamVR
  -- the runtime with OXRSys its headset.
  | .modePcvr =>
    if s.trayAnswers then
      if s.pending.isNone then some { s with warningDue := false, pending := some .ours, installing := true }
      else some { s with warningDue := false }
    else none
  | .modeSteamVr =>
    if s.trayAnswers then
      if s.pending.isNone then some { s with warningDue := false, pending := some .steamVrOnOxrsys, installing := true }
      else some { s with warningDue := false }
    else none
  | .modeStream =>
    if s.trayAnswers then
      if s.pending.isNone then some { s with warningDue := false, pending := some .stream, installing := true }
      else some { s with warningDue := false }
    else none
  | .addRuntime => if s.trayAnswers && !s.fileDialog then some { s with warningDue := false, fileDialog := true } else none
  | .addRuntimeChosen => if s.fileDialog && !s.exited then some { s with fileDialog := false } else none
  | .trayUninstall =>
    if s.trayAnswers && s.desk.packaged then some { s with warningDue := false, uninstallAsked := true } else none
  | .uninstallConfirm =>
    if s.uninstallAsked && !s.exited then
      some { s with uninstallAsked := false, exited := true, desk := s.desk.unbind }
    else none
  | .uninstallCancel => if s.uninstallAsked && !s.exited then some { s with uninstallAsked := false } else none
  | .openLogs => if s.trayAnswers then some { s with warningDue := false } else none
  | .developerMode =>
    if s.trayAnswers then
      some { s with warningDue := false, desk := { s.desk with developerMode := !s.desk.developerMode } }
    else none
  | .openRuntimeFolder => if s.trayAnswers && s.desk.developerMode then some { s with warningDue := false } else none
  | .openDriverFolder => if s.trayAnswers && s.desk.developerMode then some { s with warningDue := false } else none
  -- Quitting ends the process; an open pilot window then records its close.
  | .trayQuit => if s.trayAnswers then some { s with exited := true } else none
  -- A background change may finish after a quit: its prompt was already up.
  | .installFailed =>
    if s.pending.isSome && s.installing then some { s with pending := none, installing := false } else none
  | .elevatedDone =>
    match s.pending with
    | some p =>
        let desk := match p with
          | .bind | .ours => s.desk.swapToOxrsys
          | .other => s.desk.pickSteamVr
          | .unbind => s.desk.unbind
          | .steamVrOnOxrsys | .stream => s.desk.steamVrOnOxrsys
        -- Only the stream mode makes the desk's pilot a watcher; every other change makes it the owner.
        some { s with pending := none, installing := false,
                      desk := { desk with deskWatches := p == .stream } }
    | none => none
  | .elevatedDeclined =>
    match s.pending with
    | some p =>
        -- SteamVR's settings need no prompt: a declined unbind still hands back the headset and multiple
        -- drivers, and only the OpenXR runtime stays. A declined swap or pick changes nothing.
        some { s with pending := none, installing := false,
                      desk := if p == .unbind then s.desk.handBackHeadset else s.desk }
    | none => none
  | .windowOpen =>
    if !s.window && !s.exited && s.canOpen then some { s with window := true, opened := true, showWanted := false } else none
  | .windowError =>
    if !s.window && !s.exited && s.canOpen then
      some { s with opened := true, showWanted := false, exited := s.quitsOnClose } else none
  | .windowClose =>
    if s.window then some { s with window := false, exited := s.exited || s.quitsOnClose } else none
  | .connect | .disconnect | .autoConnect | .fieldOfView | .controllers | .pointing | .seated
  | .appearance | .conventions | .colourChecker =>
    if s.window && !s.uninstallAsked then some s else none

/-- The state after a run, or `none` at the first action the UI did not offer. -/
def run (s : State) : List Action → Option State
  | [] => some s
  | a :: rest => (step s a).bind (run · rest)

/-- A run from a desk to an action: the desk, then the actions, the last being the one reached. -/
def witness : Action → Desk × List Action
  | .launchPilot => ({}, [.launchPilot])
  | .launchTray => ({}, [.launchTray])
  | .launchWindow => ({}, [.launchWindow])
  | .launchAgent => ({}, [.launchAgent])
  | .launchShowRunning => ({ runningCopy := true }, [.launchShowRunning])
  | .launchVersion => ({}, [.launchVersion])
  | .launchUsage => ({}, [.launchUsage])
  | .deskSetup => ({}, [.launchPilot, .deskSetup])
  | .updateWarning => ({}, [.launchPilot, .deskSetup, .updateWarning])
  | .firstRunBind => ({}, [.launchPilot, .deskSetup, .firstRunBind])
  | .trayShowPilot => ({ firstRunDone := true }, [.launchTray, .deskSetup, .trayShowPilot])
  | .showRequest => ({ firstRunDone := true }, [.launchTray, .deskSetup, .showRequest])
  | .trayBind => ({ firstRunDone := true }, [.launchTray, .deskSetup, .trayBind])
  | .trayUnbind => ({ firstRunDone := true, runtime := .oxrsys }, [.launchTray, .deskSetup, .trayUnbind])
  | .runtimeOurs => ({ firstRunDone := true }, [.launchTray, .deskSetup, .runtimeOurs])
  | .runtimeOther => ({ firstRunDone := true, runtime := .oxrsys }, [.launchTray, .deskSetup, .runtimeOther])
  | .modePcvr => ({ firstRunDone := true }, [.launchTray, .deskSetup, .modePcvr])
  | .modeSteamVr => ({ firstRunDone := true }, [.launchTray, .deskSetup, .modeSteamVr])
  | .modeStream => ({ firstRunDone := true }, [.launchTray, .deskSetup, .modeStream])
  | .addRuntime => ({ firstRunDone := true }, [.launchTray, .deskSetup, .addRuntime])
  | .addRuntimeChosen => ({ firstRunDone := true }, [.launchTray, .deskSetup, .addRuntime, .addRuntimeChosen])
  | .trayUninstall => ({ firstRunDone := true, packaged := true }, [.launchTray, .deskSetup, .trayUninstall])
  | .uninstallConfirm =>
    ({ firstRunDone := true, packaged := true }, [.launchTray, .deskSetup, .trayUninstall, .uninstallConfirm])
  | .uninstallCancel =>
    ({ firstRunDone := true, packaged := true }, [.launchTray, .deskSetup, .trayUninstall, .uninstallCancel])
  | .openLogs => ({ firstRunDone := true }, [.launchTray, .deskSetup, .openLogs])
  | .developerMode => ({ firstRunDone := true }, [.launchTray, .deskSetup, .developerMode])
  | .openRuntimeFolder =>
    ({ firstRunDone := true }, [.launchTray, .deskSetup, .developerMode, .openRuntimeFolder])
  | .openDriverFolder =>
    ({ firstRunDone := true }, [.launchTray, .deskSetup, .developerMode, .openDriverFolder])
  | .trayQuit => ({ firstRunDone := true }, [.launchTray, .deskSetup, .trayQuit])
  | .installFailed => ({}, [.launchPilot, .deskSetup, .firstRunBind, .installFailed])
  | .elevatedDone => ({ firstRunDone := true }, [.launchTray, .deskSetup, .runtimeOurs, .elevatedDone])
  | .elevatedDeclined => ({ firstRunDone := true }, [.launchTray, .deskSetup, .runtimeOurs, .elevatedDeclined])
  | .windowOpen => ({}, [.launchWindow, .windowOpen])
  | .windowError => ({}, [.launchWindow, .windowError])
  | .windowClose => ({}, [.launchWindow, .windowOpen, .windowClose])
  | .connect => ({}, [.launchWindow, .windowOpen, .connect])
  | .disconnect => ({}, [.launchWindow, .windowOpen, .disconnect])
  | .autoConnect => ({}, [.launchWindow, .windowOpen, .autoConnect])
  | .fieldOfView => ({}, [.launchWindow, .windowOpen, .fieldOfView])
  | .controllers => ({}, [.launchWindow, .windowOpen, .controllers])
  | .pointing => ({}, [.launchWindow, .windowOpen, .pointing])
  | .seated => ({}, [.launchWindow, .windowOpen, .seated])
  | .appearance => ({}, [.launchWindow, .windowOpen, .appearance])
  | .conventions => ({}, [.launchWindow, .windowOpen, .conventions])
  | .colourChecker => ({}, [.launchWindow, .windowOpen, .colourChecker])

/-- `witness a` is valid and ends in `a`. -/
def reaches (a : Action) : Bool :=
  let (d, path) := witness a
  (run (.start d) path).isSome && path.getLast? == some a

/-- Every action is in `Action.all`. -/
theorem all_complete (a : Action) : a ∈ Action.all := by
  cases a <;> decide

/-- Every action the UI has is reachable from some desk. -/
theorem every_action_reachable (a : Action) : reaches a = true := by
  cases a <;> decide

/-- A first start on a fresh desk binds OXRSys and opens the pilot; nothing else has to be found. -/
def firstStart : List Action := [.launchPilot, .deskSetup, .firstRunBind, .installFailed, .windowOpen]
def firstStartBound : List Action := [.launchPilot, .deskSetup, .firstRunBind, .elevatedDone, .windowOpen]

/-- The first-run bind's prompt does not hold the pilot back: the pilot opens while the bind is pending. -/
def firstStartPromptPending : List Action := [.launchPilot, .deskSetup, .firstRunBind, .windowOpen]

theorem first_start_opens_the_pilot :
    ((run (.start {}) firstStartBound).map (·.window)) = some true ∧
    ((run (.start {}) firstStartPromptPending).map (·.window)) = some true := by
  decide

/-- With no tray icon, closing a desk copy's pilot quits rather than leave a process nothing can reach. -/
theorem no_tray_icon_close_quits :
    ((run (.start { trayIcon := false, firstRunDone := true })
        [.launchPilot, .deskSetup, .windowOpen, .windowClose]).map (·.exited)) = some true := by
  decide

/-- With no tray icon, `--tray` still opens the pilot (nothing else could), and closing it quits. -/
theorem no_tray_icon_tray_start_opens_then_quits :
    ((run (.start { trayIcon := false, firstRunDone := true })
        [.launchTray, .deskSetup, .windowOpen, .windowClose]).map (·.exited)) = some true := by
  decide

/-- Quitting from the tray with the pilot open records the window's close after the quit, and a
prompt left up at the quit still finishes. -/
theorem quit_with_the_pilot_open :
    ((run (.start { firstRunDone := true })
        [.launchPilot, .deskSetup, .windowOpen, .trayBind, .trayQuit, .windowClose, .elevatedDone]).map
        (·.exited)) = some true := by
  decide

/-- An agent's pilot never reaches the tray: its menu belongs to the desk copy. -/
theorem agent_has_no_tray :
    run (.start { firstRunDone := true }) [.launchAgent, .deskSetup] = none ∧
    run (.start { firstRunDone := true }) [.launchAgent, .windowOpen, .trayBind] = none := by
  decide

/-! ## The two tracks -/

/-- Swap to OXRSys: from the tray's Bind, or by picking OXRSys in the runtime menu. -/
def swapTracks : List (List Action) :=
  [[.launchTray, .deskSetup, .trayBind, .elevatedDone],
   [.launchTray, .deskSetup, .runtimeOurs, .elevatedDone],
   [.launchPilot, .deskSetup, .firstRunBind, .elevatedDone]]

/-- Back to SteamVR: Unbind, or picking SteamVR in the runtime menu. -/
def backTracks : List (List Action) :=
  [[.launchTray, .deskSetup, .trayUnbind, .elevatedDone],
   [.launchTray, .deskSetup, .runtimeOther, .elevatedDone]]

/-- Where a track ends from a desk, if the UI offers every step there. -/
def trackEnd (d : Desk) (track : List Action) : Option Desk := (run (.start d) track).map (·.desk)

/-! ## The tray's three modes -/

/-- PCVR on OXRSys, SteamVR on OXRSys, and streaming to a headset (the Steam Frame, a Quest 3). -/
def modeTracks : List (List Action) :=
  [[.launchTray, .deskSetup, .modePcvr, .elevatedDone],
   [.launchTray, .deskSetup, .modeSteamVr, .elevatedDone],
   [.launchTray, .deskSetup, .modeStream, .elevatedDone]]

/-- Each mode's row: OXRSys the runtime with the pilot owning; SteamVR the runtime on OXRSys with the
pilot owning; the same with the pilot watching. -/
def Desk.inMode (d : Desk) : Nat → Bool
  | 0 => d.onOxrsys && !d.deskWatches
  | 1 => d.onSteamVrWithOxrsys && !d.deskWatches
  | _ => d.onSteamVrWithOxrsys && d.deskWatches

/-- Each mode, accepted, ends in its row from any desk where the tray answers. -/
theorem modes_end_in_their_rows (d : Desk) (fresh : d.firstRunDone = true) (icon : d.trayIcon = true)
    (alone : d.runningCopy = false) :
    ((trackEnd d [.launchTray, .deskSetup, .modePcvr, .elevatedDone]).map (·.inMode 0)) = some true ∧
    ((trackEnd d [.launchTray, .deskSetup, .modeSteamVr, .elevatedDone]).map (·.inMode 1)) = some true ∧
    ((trackEnd d [.launchTray, .deskSetup, .modeStream, .elevatedDone]).map (·.inMode 2)) = some true := by
  simp [trackEnd, run, step, State.start, State.ownsDesk, State.trayAnswers, Desk.inMode, Desk.swapToOxrsys,
    Desk.steamVrOnOxrsys, Desk.onOxrsys, Desk.onSteamVrWithOxrsys, fresh, icon, alone]

/-- Only the stream mode makes the desk's pilot a watcher: no other step turns it on. -/
theorem only_the_stream_mode_watches (s t : State) (a : Action) (h : step s a = some t)
    (watching : t.desk.deskWatches = true) :
    s.desk.deskWatches = true ∨ (a = .elevatedDone ∧ s.pending = some .stream) := by
  cases a <;> simp only [step] at h <;> (repeat' split at h) <;> simp_all <;> subst_vars <;>
    simp_all [Desk.unbind, Desk.handBackHeadset]

/-- The first start's swap and both ways back, on a fresh desk running SteamVR. -/
theorem fresh_desk_tracks :
    ((trackEnd {} [.launchPilot, .deskSetup, .firstRunBind, .elevatedDone]).map (·.onOxrsys)) = some true ∧
    ((trackEnd { firstRunDone := true, runtime := .oxrsys, previous := .steamVr, forcedOxrsys := true,
                 multipleDrivers := true, multipleDriversBefore := some false }
        [.launchTray, .deskSetup, .trayUnbind, .elevatedDone]).map (·.onSteamVr)) = some true := by
  decide

/-! ## Checking recorded runs -/

/-- One `ui.*` span of a recorded session. -/
structure Span where
  session : Int
  name : String
  ok : Bool
  deriving Repr, Inhabited

def Span.ofJson? (j : Json) : Option Span := do
  let session ← (j.getObjValAs? Int "session").toOption
  let name ← (j.getObjValAs? String "span").toOption
  let ok ← (j.getObjValAs? Bool "ok").toOption
  return { session, name, ok }

/-- Every desk a session could have started on. -/
def Desk.all : List Desk := Id.run do
  let mut out := []
  let runtimes := [Runtime.steamVr, .oxrsys, .other, .none]
  for packaged in [false, true] do
    for runningCopy in [false, true] do
      for firstRunDone in [false, true] do
        for runtime in runtimes do
          for previous in runtimes do
            for forcedOxrsys in [false, true] do
              for multipleDrivers in [false, true] do
                for multipleDriversBefore in [Option.none, some false, some true] do
                  for oxrsysDriver in [false, true] do
                    for developerMode in [false, true] do
                      for trayIcon in [false, true] do
                        for deskWatches in [false, true] do
                          out := { packaged, runningCopy, firstRunDone, runtime, previous, forcedOxrsys,
                                   multipleDrivers, multipleDriversBefore, oxrsysDriver, developerMode, trayIcon,
                                   deskWatches } :: out
  return out.reverse

/-- Replays one session's spans, in the order they started, from every desk at once; accepts the
session when some desk takes every span as an enabled step. Names that are not `ui.*` are skipped. -/
def checkSession (spans : List Span) : Except String Unit := do
  let mut states : List State := Desk.all.map State.start
  let mut index := 0
  for sp in spans do
    index := index + 1
    if !sp.name.startsWith "ui." then continue
    let some a := Action.ofSpanName? sp.name
      | throw s!"span {index}: {sp.name} is not an action of the model"
    let next := states.filterMap (step · a)
    if next.isEmpty then
      throw s!"span {index}: {sp.name} is not offered after the spans before it"
    states := next
  return ()

/-- Splits a JSONL export into sessions and checks each; returns the sessions checked. -/
def checkJsonl (text : String) : Except String Nat := do
  let mut sessions : Array (Int × Array Span) := #[]
  let mut line := 0
  for raw in text.splitOn "\n" do
    line := line + 1
    let l := raw.trimAscii.toString
    if l.isEmpty then continue
    let j ← (Json.parse l).mapError (s!"line {line}: " ++ ·)
    let some sp := Span.ofJson? j | throw s!"line {line}: needs session, span and ok"
    match sessions.findIdx? (·.1 == sp.session) with
    | some i => sessions := sessions.modify i fun (k, v) => (k, v.push sp)
    | none => sessions := sessions.push (sp.session, #[sp])
  for (k, spans) in sessions do
    (checkSession spans.toList).mapError (s!"session {k}: " ++ ·) |> liftExcept
  return sessions.size
where
  liftExcept {α} (e : Except String α) : Except String α := e

/-! ## The C++ header -/

def emitCpp : String := Id.run do
  let mut out := "// SPDX-License-Identifier: Apache-2.0 OR MIT\n//\n"
  out := out ++ "// Generated by `lake exe emit_ui_spans` from lean/XrPilot/Ui.lean; do not edit.\n"
  out := out ++ "// Every UI action, and the span it records. lean/XrPilot/Ui.lean proves each reachable and checks\n"
  out := out ++ "// recorded runs against the same model.\n\n#pragma once\n\nnamespace xrpilot\n{\n\nenum class UiAction\n{\n"
  for a in Action.all do
    out := out ++ s!"    {a.cppName},\n"
  out := out ++ "};\n\ninline const char* uiSpanName(UiAction action)\n{\n    switch (action)\n    {\n"
  for a in Action.all do
    out := out ++ s!"    case UiAction::{a.cppName}:\n        return \"{a.spanName}\";\n"
  out := out ++ "    }\n    return \"ui.unknown\";\n}\n\n} // namespace xrpilot\n"
  return out

end XrPilot.Ui
