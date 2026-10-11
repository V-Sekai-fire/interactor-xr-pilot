/-
The OXRSys stream's control plane as state machines: the runtime's spectator list (a queue kept alive
by heartbeats), the runtime's session (who owns it, who watches), and XR Pilot's spectator heartbeat.

* The list keeps one entry per address (`refresh_keeps_one_each`), and expiry keeps exactly the
  spectators heard within the timeout (`expire_keeps_the_fresh`).
* A spectator never takes the session (`spectator_connect_never_owns`), only the owner's goodbye ends
  it (`only_the_owner_ends_the_session`), and the owner is never also a spectator, so it never gets the
  video twice (`owner_is_not_watching`).
* A spectator heard last at `t` gets no video from `t + spectatorTimeoutMs + receiveTimeoutMs` on, since
  the control loop expires the list every time round, packet or not (`stale_spectator_dropped`). The
  loop as first written expired it only when a packet arrived; `packetOnlyExpiry_keeps_a_stale_spectator`
  is the run that shows it.
* One lost heartbeat keeps a spectator; two may drop it (`one_loss_kept`, `two_losses_may_drop`).
* XR Pilot as a spectator sends only its heartbeat and its goodbye (`spectator_sends_only_connects`),
  at most once per `heartbeatMs`.

`emitCpp` writes `oxrsys/tests/StreamModelCases.h`: the routing table and list runs this model works
out, which `TestStreamingSpectators.cpp` replays against `StreamingSpectators.h`.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/

namespace XrPilot.Stream

/-! Times are milliseconds on the runtime's steady clock. -/

/-- XR Pilot repeats its spectator `ClientConnect` this often (`SpectatorHeartbeatNs`). -/
def heartbeatMs : Nat := 2000
/-- The runtime drops a spectator unheard this long (`SPECTATOR_TIMEOUT_MS`). -/
def spectatorTimeoutMs : Nat := 5000
/-- The control loop's receive timeout: it goes round at least this often with no packet. -/
def receiveTimeoutMs : Nat := 1000

/-- A client's control address. -/
structure Addr where
  ip : Nat
  port : Nat
  deriving DecidableEq, Repr, Inhabited

structure Entry where
  addr : Addr
  lastSeen : Nat
  deriving DecidableEq, Repr, Inhabited

/-! ## The spectator list -/

def addrs (l : List Entry) : List Addr := l.map (·.addr)

/-- `List::Refresh`: a heartbeat; true when the spectator is new, which joins the back of the list. -/
def refresh (l : List Entry) (a : Addr) (now : Nat) : List Entry × Bool :=
  if a ∈ addrs l then (l.map fun e => if e.addr = a then { e with lastSeen := now } else e, false)
  else (l ++ [⟨a, now⟩], true)

/-- `List::Remove`: true when it was there. -/
def remove (l : List Entry) (a : Addr) : List Entry × Bool :=
  if a ∈ addrs l then (l.filter (·.addr != a), true) else (l, false)

/-- Heard within the timeout. A clock behind `lastSeen` reads as just heard, as a negative duration does. -/
def fresh (now timeout : Nat) (e : Entry) : Bool := now - e.lastSeen < timeout

/-- `List::Expire`: the spectators kept, and the addresses dropped, in list order. -/
def expire (l : List Entry) (now timeout : Nat) : List Entry × List Addr :=
  (l.filter (fresh now timeout), addrs (l.filter (fun e => !fresh now timeout e)))

theorem addrs_map_keeps (l : List Entry) (f : Entry → Entry) (h : ∀ e, (f e).addr = e.addr) :
    addrs (l.map f) = addrs l := by
  induction l with
  | nil => rfl
  | cons e rest ih => simp [addrs, h] at ih ⊢

theorem refresh_contains (l : List Entry) (a : Addr) (now : Nat) : a ∈ addrs (refresh l a now).1 := by
  unfold refresh
  split
  · rw [addrs_map_keeps _ _ (by intro e; split <;> simp_all)]; assumption
  · simp [addrs]

/-- One entry per address: a heartbeat never adds a second. -/
theorem refresh_keeps_one_each (l : List Entry) (a : Addr) (now : Nat) (h : (addrs l).Nodup) :
    (addrs (refresh l a now).1).Nodup := by
  unfold refresh
  split
  · rw [addrs_map_keeps _ _ (by intro e; split <;> simp_all)]; exact h
  · rename_i hn
    simp only [addrs, List.map_append, List.map_cons, List.map_nil] at hn ⊢
    exact List.nodup_append.mpr ⟨h, by simp, by
      intro x hx y hy; simp at hy; subst hy; intro hxy; subst hxy; exact hn hx⟩

theorem remove_drops (l : List Entry) (a : Addr) : a ∉ addrs (remove l a).1 := by
  unfold remove
  split
  · simp [addrs]
  · assumption

theorem sublist_nodup {l : List Entry} (p : Entry → Bool) (h : (addrs l).Nodup) :
    (addrs (l.filter p)).Nodup :=
  List.Nodup.sublist (List.Sublist.map _ (List.filter_sublist)) h

theorem remove_keeps_one_each (l : List Entry) (a : Addr) (h : (addrs l).Nodup) :
    (addrs (remove l a).1).Nodup := by
  unfold remove; split
  · exact sublist_nodup _ h
  · exact h

/-- Expiry keeps exactly the spectators heard within the timeout. -/
theorem expire_keeps_the_fresh (l : List Entry) (now timeout : Nat) (e : Entry) :
    e ∈ (expire l now timeout).1 ↔ e ∈ l ∧ now - e.lastSeen < timeout := by
  simp [expire, fresh]

/-! ## The runtime's session -/

/-- What arrives on the control port. -/
inductive Packet where
  /-- `ClientConnect`, with `CLIENT_CAPABILITY_SPECTATOR` or without. -/
  | connect (spectator : Bool)
  /-- `ServerDisconnect`. -/
  | bye
  /-- Latency reports, keyframe requests, NACKs and the rest. -/
  | payload
  deriving DecidableEq, Repr, Inhabited

/-- What the runtime does with a control packet, from who sent it. -/
inductive Route where
  /-- A plain connect: the sender owns the session, and stops watching if it was. -/
  | takeSession
  /-- A spectator connect: the sender watches, or keeps watching. -/
  | watch
  /-- A spectator connect from the owner: its session ends, and it watches. -/
  | stepDownToWatch
  | endSession
  | leaveWatching
  | handle
  | ignore
  deriving DecidableEq, Repr, Inhabited

/-- `streaming_spectators::RouteControl`. -/
def route (p : Packet) (isOwner isSpectator : Bool) : Route :=
  match p with
  | .connect true => if isOwner then .stepDownToWatch else .watch
  | .connect false => .takeSession
  | .bye => if isSpectator then .leaveWatching else if isOwner then .endSession else .ignore
  | .payload => if isSpectator then .ignore else .handle

structure Server where
  owner : Option Addr := none
  spectators : List Entry := []
  deriving DecidableEq, Repr, Inhabited

/-- What happens on the control loop's turn: a packet, or the receive timeout with none. -/
inductive Event where
  | control (sender : Addr) (p : Packet) (now : Nat)
  | idle (now : Nat)
  deriving DecidableEq, Repr, Inhabited

def Event.now : Event → Nat
  | .control _ _ now | .idle now => now

def Server.expireAt (s : Server) (now : Nat) : Server :=
  { s with spectators := (expire s.spectators now spectatorTimeoutMs).1 }

def Server.apply (s : Server) (a : Addr) (p : Packet) (now : Nat) : Server :=
  match route p (s.owner == some a) (a ∈ addrs s.spectators) with
  | .takeSession => { owner := some a, spectators := (remove s.spectators a).1 }
  | .watch => { s with spectators := (refresh s.spectators a now).1 }
  | .stepDownToWatch => { owner := none, spectators := (refresh s.spectators a now).1 }
  | .endSession => { s with owner := none }
  | .leaveWatching => { s with spectators := (remove s.spectators a).1 }
  | .handle | .ignore => s

/-- One turn of the control loop: it expires the list every turn, then routes the packet if any. -/
def step (s : Server) : Event → Server
  | .idle now => s.expireAt now
  | .control a p now => (s.expireAt now).apply a p now

/-- The loop as first written: it expired the list only when a packet arrived. -/
def stepPacketOnlyExpiry (s : Server) : Event → Server
  | .idle _ => s
  | .control a p now => (s.expireAt now).apply a p now

/-- Who gets each video packet: the owner, then each spectator; nobody without a session. -/
def Server.videoTargets (s : Server) : List Addr :=
  match s.owner with
  | some o => o :: addrs s.spectators
  | none => []

/-- The owner is not also watching, and the list has one entry per address. -/
def Server.Ok (s : Server) : Prop :=
  (addrs s.spectators).Nodup ∧ ∀ o, s.owner = some o → o ∉ addrs s.spectators

theorem expireAt_ok (s : Server) (now : Nat) (h : s.Ok) : (s.expireAt now).Ok := by
  obtain ⟨nd, own⟩ := h
  refine ⟨sublist_nodup _ nd, fun o ho hin => own o ho ?_⟩
  exact (List.Sublist.map _ List.filter_sublist).subset hin

theorem apply_ok (s : Server) (a : Addr) (p : Packet) (now : Nat) (h : s.Ok) : (s.apply a p now).Ok := by
  obtain ⟨nd, own⟩ := h
  unfold Server.apply
  split
  · exact ⟨remove_keeps_one_each _ _ nd, fun o ho => by cases ho; exact remove_drops _ _⟩
  · rename_i hr
    refine ⟨refresh_keeps_one_each _ _ _ nd, fun o ho hin => ?_⟩
    -- a watcher that is not the owner joins; the owner is untouched and was not watching
    have : o ≠ a := by
      intro hoa; subst hoa
      cases p with
      | connect sp => cases sp <;> simp_all [route]
      | bye => simp [route] at hr; split at hr <;> simp_all
      | payload => simp [route] at hr; split at hr <;> simp_all
    unfold refresh at hin
    split at hin
    · rw [addrs_map_keeps _ _ (by intro e; split <;> simp_all)] at hin; exact own o ho hin
    · simp [addrs] at hin
      rcases hin with hin | hin
      · exact own o ho (by simpa [addrs] using hin)
      · exact this hin
  · exact ⟨refresh_keeps_one_each _ _ _ nd, fun o ho => by cases ho⟩
  · exact ⟨nd, fun o ho => by cases ho⟩
  · refine ⟨remove_keeps_one_each _ _ nd, fun o ho hin => own o ho ?_⟩
    unfold remove at hin; split at hin
    · exact (List.Sublist.map _ List.filter_sublist).subset hin
    · exact hin
  · exact ⟨nd, own⟩
  · exact ⟨nd, own⟩

/-- The owner is never also a spectator, from any run: it never gets the video twice. -/
theorem owner_is_not_watching (s : Server) (e : Event) (h : s.Ok) : (step s e).Ok := by
  cases e with
  | idle now => exact expireAt_ok s now h
  | control a p now => exact apply_ok _ a p now (expireAt_ok s now h)

/-- A spectator connect never makes its sender the owner, and never moves another owner's session. -/
theorem spectator_connect_never_owns (s : Server) (a : Addr) (now : Nat) :
    (step s (.control a (.connect true) now)).owner ≠ some a ∧
    (∀ o, s.owner = some o → o ≠ a → (step s (.control a (.connect true) now)).owner = some o) := by
  constructor
  · by_cases ho : s.owner = some a <;> simp [step, Server.apply, Server.expireAt, route, ho, refresh]
  · intro o ho hne
    have : s.owner ≠ some a := by rw [ho]; simp [hne]
    simp [step, Server.apply, Server.expireAt, route, ho, hne]

/-- Only the owner's goodbye ends the session: anyone else's leaves the owner as it was. -/
theorem only_the_owner_ends_the_session (s : Server) (a : Addr) (now : Nat) (o : Addr)
    (ho : s.owner = some o) (hne : o ≠ a) :
    (step s (.control a .bye now)).owner = some o := by
  simp only [step, Server.apply, Server.expireAt, route]
  split <;> simp_all <;> split at * <;> simp_all

/-- A turn at least the timeout after a spectator's last heartbeat drops it, unless that turn is its
heartbeat. -/
theorem stale_spectator_dropped (s : Server) (e : Event) (x : Entry) (late : e.now - x.lastSeen ≥ spectatorTimeoutMs)
    (notIt : ∀ p, e ≠ .control x.addr p e.now) :
    x ∉ (step s e).spectators := by
  have gone : x ∉ (s.expireAt e.now).spectators := by
    intro hin
    have := (List.mem_filter.mp hin).2
    simp [fresh] at this
    omega
  cases e with
  | idle now => exact gone
  | control a p now =>
    have ha : a ≠ x.addr := by intro h; subst h; exact notIt p rfl
    simp only [step, Server.apply]
    split
    · intro hin; unfold remove at hin; split at hin
      · exact gone (List.mem_filter.mp hin).1
      · exact gone hin
    · intro hin; unfold refresh at hin; split at hin
      · obtain ⟨y, hy, hyx⟩ := List.mem_map.mp hin
        split at hyx
        · subst hyx; simp at ha; exact ha (by simp_all)
        · subst hyx; exact gone hy
      · simp at hin; rcases hin with hin | hin
        · exact gone hin
        · subst hin; exact ha rfl
    · intro hin; unfold refresh at hin; split at hin
      · obtain ⟨y, hy, hyx⟩ := List.mem_map.mp hin
        split at hyx
        · subst hyx; simp at ha; exact ha (by simp_all)
        · subst hyx; exact gone hy
      · simp at hin; rcases hin with hin | hin
        · exact gone hin
        · subst hin; exact ha rfl
    · exact gone
    · intro hin; unfold remove at hin; split at hin
      · exact gone (List.mem_filter.mp hin).1
      · exact gone hin
    · exact gone
    · exact gone

/-! ## Runs -/

def run (stepFn : Server → Event → Server) (s : Server) (events : List Event) : Server :=
  events.foldl stepFn s

def frame : Addr := ⟨0xC0A801C2, 40438⟩ -- the Steam Frame, 192.168.1.194
def desk : Addr := ⟨0x7F000001, 51000⟩ -- XR Pilot on loopback

/-- The Frame owns the session, the desk watches and then quits without its goodbye, and the Frame
sends nothing on the control port for a minute (it was streaming, so tracking went elsewhere). -/
def deskVanishes : List Event :=
  [.control frame (.connect false) 0, .control desk (.connect true) 100] ++
    (List.range 60).map fun i => .idle (1100 + 1000 * i)

/-- The loop as first written keeps sending the video to the vanished desk for the whole minute. -/
theorem packetOnlyExpiry_keeps_a_stale_spectator :
    desk ∈ (run stepPacketOnlyExpiry {} deskVanishes).videoTargets := by decide +kernel

/-- The loop as it is drops it within the timeout and one turn. -/
theorem every_turn_expiry_drops_it :
    desk ∉ (run step {} deskVanishes).videoTargets ∧
    desk ∈ (run step {} (deskVanishes.take 6)).videoTargets ∧
    desk ∉ (run step {} (deskVanishes.take 7)).videoTargets := by decide +kernel

/-- Heartbeats every `heartbeatMs` from `start`, with the ones at `lost` indexes not arriving, and the
loop going round every `receiveTimeoutMs` in between. -/
def heartbeats (start count : Nat) (lost : List Nat) : List Event :=
  [.control frame (.connect false) 0] ++
    ((List.range (count * 2)).filterMap fun i =>
      let now := start + receiveTimeoutMs * i
      if i % 2 == 0 && !(lost.contains (i / 2)) then some (.control desk (.connect true) now)
      else some (.idle now))

def watchingThroughout (events : List Event) : Bool :=
  (List.range (events.length - 1)).all fun n => desk ∈ addrs (run step {} (events.take (n + 2))).spectators

/-- One lost heartbeat in a row keeps the spectator: 2 × 2 s < 5 s. -/
theorem one_loss_kept : watchingThroughout (heartbeats 100 8 [2, 5]) = true := by decide +kernel

/-- Two in a row may drop it: 3 × 2 s ≥ 5 s, so the margin is one loss. -/
theorem two_losses_may_drop : watchingThroughout (heartbeats 100 8 [2, 3]) = false := by decide +kernel

theorem heartbeat_margin : 2 * heartbeatMs < spectatorTimeoutMs ∧ 3 * heartbeatMs ≥ spectatorTimeoutMs := by
  decide

/-! ## XR Pilot as a spectator -/

structure Client where
  spectator : Bool := false
  connected : Bool := false
  autoConnect : Bool := false
  connectRequested : Bool := false
  /-- The last spectator heartbeat; `none` sends at the next tick. -/
  lastHeartbeat : Option Nat := none
  deriving DecidableEq, Repr, Inhabited

/-- What the client sends. -/
inductive Send where
  | connect (spectator : Bool)
  | bye
  | tracking
  | body
  | keyframeRequest
  | latencyReport
  deriving DecidableEq, Repr, Inhabited

inductive ClientEvent where
  /-- The tracking loop's tick. -/
  | tick (now : Nat)
  /-- A decoded frame wants a keyframe, or a latency report is due. -/
  | wantKeyframe
  | wantLatencyReport
  | userConnect
  | userDisconnect
  | setAutoConnect (on : Bool)
  | setSpectator (on : Bool)
  deriving DecidableEq, Repr, Inhabited

/-- `Client::disconnect`: turns auto-connect off, and says goodbye when connected. -/
def Client.disconnect (c : Client) : Client × List Send :=
  ({ c with autoConnect := false, connectRequested := false, connected := false },
   if c.connected then [.bye] else [])

/-- A spectator wants to watch: it is, or auto-connect or the Connect button asks it to. -/
def Client.wantsWatching (c : Client) : Bool := c.connected || c.autoConnect || c.connectRequested

/-- `heartbeatMs` has passed since the last heartbeat, or none was sent since the mode changed. -/
def Client.heartbeatDue (c : Client) (now : Nat) : Bool :=
  match c.lastHeartbeat with
  | none => true
  | some t => now - t ≥ heartbeatMs

/-- One client step; a client that is not a spectator is out of this model's scope beyond what it sends. -/
def Client.step (c : Client) : ClientEvent → Client × List Send
  | .tick now =>
    if c.spectator then
      if c.wantsWatching && c.heartbeatDue now then
        ({ c with lastHeartbeat := some now, connectRequested := false, connected := true }, [.connect true])
      else (c, [])
    else (c, if c.connected then [.tracking, .body] else [])
  | .wantKeyframe => (c, if c.connected && !c.spectator then [.keyframeRequest] else [])
  | .wantLatencyReport => (c, if c.connected && !c.spectator then [.latencyReport] else [])
  | .userConnect => ({ c with connectRequested := true }, [])
  | .userDisconnect => c.disconnect
  | .setAutoConnect on => ({ c with autoConnect := on }, [])
  | .setSpectator on =>
    let (d, out) := c.disconnect
    ({ d with autoConnect := c.autoConnect, spectator := on, lastHeartbeat := none }, out)

/-- A spectator sends only its heartbeat and its goodbye: no tracking, body, keyframe requests or
latency reports. -/
theorem spectator_sends_only_connects (c : Client) (e : ClientEvent) (h : c.spectator = true)
    (notSwitch : ∀ on, e ≠ .setSpectator on) :
    ∀ m ∈ (c.step e).2, m = .connect true ∨ m = .bye := by
  intro m hm
  cases e with
  | tick now =>
    simp only [Client.step, h, ↓reduceIte] at hm
    split at hm
    · simp at hm; exact Or.inl hm
    · simp at hm
  | setSpectator on => exact absurd rfl (notSwitch on)
  | userDisconnect => simp [Client.step, Client.disconnect] at hm; exact Or.inr hm.2
  | _ => simp_all [Client.step]

/-- Switching mode keeps auto-connect, and says goodbye only when connected. -/
theorem switching_keeps_auto_connect (c : Client) (on : Bool) :
    (c.step (.setSpectator on)).1.autoConnect = c.autoConnect ∧
    (c.step (.setSpectator on)).1.spectator = on ∧
    ((c.step (.setSpectator on)).2 = if c.connected then [.bye] else []) := by
  simp [Client.step, Client.disconnect]

/-- Two heartbeats are at least `heartbeatMs` apart. -/
theorem heartbeat_paced (c : Client) (now t : Nat) (sp : c.spectator = true) (h : c.lastHeartbeat = some t)
    (early : now - t < heartbeatMs) : (c.step (.tick now)).2 = [] := by
  have : c.heartbeatDue now = false := by simp [Client.heartbeatDue, h]; omega
  simp [Client.step, sp, this]

/-! ## Cases for the C++ -/

/-- One operation on `List`, and what this model works out it returns and leaves. -/
inductive ListOp where
  | refresh (a : Addr) (now : Nat)
  | remove (a : Addr)
  | expire (now : Nat)
  deriving DecidableEq, Repr, Inhabited

def cppAddr (a : Addr) : String := s!"\{{a.ip}u, {a.port}}"

def addrText (l : List Addr) : String := ",".intercalate (l.map fun a => s!"{a.ip}:{a.port}")

/-- The runs the C++ replays: a heartbeat with one loss, two losses, goodbyes, a repeated refresh, the
exact timeout boundary, and a clock behind the last heartbeat. -/
def listRuns : List (String × List ListOp) :=
  let b : Addr := ⟨0xC0A801E6, 51001⟩
  [("one lost heartbeat is kept",
     [.refresh desk 0, .refresh desk 2000, .expire 4000, .expire 6000, .refresh desk 6000, .expire 10999]),
   ("two lost heartbeats drop it", [.refresh desk 0, .refresh desk 2000, .expire 6999, .expire 7000]),
   ("a goodbye removes only its sender", [.refresh desk 0, .refresh b 0, .remove desk, .remove desk, .expire 1]),
   ("a repeated refresh keeps one entry and its place",
     [.refresh desk 0, .refresh b 10, .refresh desk 20, .expire 5005, .expire 5010]),
   ("the timeout is exact", [.refresh b 1000, .expire 5999, .expire 6000]),
   ("a clock behind the last heartbeat reads as just heard", [.refresh desk 9000, .expire 100])]

/-- Runs one op: its boolean (refresh: new; remove: removed; expire: whether any dropped), the
addresses dropped, and the list after. -/
def ListOp.run (l : List Entry) : ListOp → Bool × List Addr × List Entry
  | .refresh a now => let (l', n) := Stream.refresh l a now; (n, [], l')
  | .remove a => let (l', r) := Stream.remove l a; (r, [], l')
  | .expire now => let (l', d) := Stream.expire l now spectatorTimeoutMs; (!d.isEmpty, d, l')

def emitListRun (name : String) (ops : List ListOp) : String := Id.run do
  let mut l : List Entry := []
  let mut out := s!"    \{\"{name}\",\n     \{"
  for op in ops do
    let (b, dropped, l') := op.run l
    l := l'
    let (kind, a, now) := match op with
      | .refresh a now => ("Refresh", a, now)
      | .remove a => ("Remove", a, 0)
      | .expire now => ("Expire", default, now)
    out := out ++ s!"\n      \{ListOpKind::{kind}, {cppAddr a}, {now}, {b}, \"{addrText dropped}\", \"{addrText (addrs l)}\"},"
  return out ++ "\n     }},\n"

def Packet.cpp : Packet → String
  | .connect true => "SpectatorConnect"
  | .connect false => "Connect"
  | .bye => "Bye"
  | .payload => "Payload"

def Route.cpp : Route → String
  | .takeSession => "TakeSession"
  | .watch => "Watch"
  | .stepDownToWatch => "StepDownToWatch"
  | .endSession => "EndSession"
  | .leaveWatching => "LeaveWatching"
  | .handle => "Handle"
  | .ignore => "Ignore"

def Packet.all : List Packet := [.connect true, .connect false, .bye, .payload]

def emitCpp : String := Id.run do
  let mut out := "// SPDX-License-Identifier: MPL-2.0\n//\n"
  out := out ++ "// Generated by `lake exe emit_stream_cases` from lean/XrPilot/Stream.lean; do not edit.\n"
  out := out ++ "// What the model works out for the runtime's control routing and its spectator list;\n"
  out := out ++ "// TestStreamingSpectators.cpp replays each against StreamingSpectators.h.\n\n#pragma once\n\n"
  out := out ++ "#include \"StreamingSpectators.h\"\n\n#include <cstdint>\n#include <vector>\n\n"
  out := out ++ "namespace stream_model\n{\n\nusing oxrsys::streaming_spectators::Address;\n"
  out := out ++ "using oxrsys::streaming_spectators::ControlPacket;\nusing oxrsys::streaming_spectators::ControlRoute;\n\n"
  out := out ++ "struct RouteCase\n{\n    ControlPacket packet;\n    bool isOwner;\n    bool isSpectator;\n    ControlRoute route;\n};\n\n"
  out := out ++ "inline const RouteCase routeCases[] = {\n"
  for p in Packet.all do
    for o in [false, true] do
      for sp in [false, true] do
        out := out ++ s!"    \{ControlPacket::{p.cpp}, {o}, {sp}, ControlRoute::{(route p o sp).cpp}},\n"
  out := out ++ "};\n\nenum class ListOpKind\n{\n    Refresh,\n    Remove,\n    Expire,\n};\n\n"
  out := out ++ "struct ListOp\n{\n    ListOpKind kind;\n    Address address;\n    int64_t nowMs;\n    bool result;\n"
  out := out ++ "    const char* dropped;\n    const char* after;\n};\n\n"
  out := out ++ "struct ListRun\n{\n    const char* name;\n    std::vector<ListOp> ops;\n};\n\n"
  out := out ++ s!"constexpr int64_t spectatorTimeoutMs = {spectatorTimeoutMs};\n\n"
  out := out ++ "inline const ListRun listRuns[] = {\n"
  for (name, ops) in listRuns do
    out := out ++ emitListRun name ops
  out := out ++ "};\n\n} // namespace stream_model\n"
  return out

end XrPilot.Stream
