package shm

import "testing"

// TestWaitLimitAdaptation pins the adaptive-limit bookkeeping that Wait and
// WaitState share through recordOutcome: a spin-resolved wait widens the window
// by IncStep, a fall-through narrows it by DecStep, and both clamp to the
// [min, max] band. This is a characterization test — it locks the behavior the
// recordOutcome extraction must preserve.
func TestWaitLimitAdaptation(t *testing.T) {
	// Hit: condition true immediately -> grow by IncStep, sleepAction untouched.
	w := &WaitStrategy{CurrentLimit: 1000}
	if !w.Wait(func() bool { return true }, func() { t.Fatal("sleepAction must not run on a hit") }) {
		t.Fatal("Wait: expected ready on an always-true condition")
	}
	if want := int32(1000) + waitStrategyIncStep; w.CurrentLimit != want {
		t.Errorf("Wait hit grow: CurrentLimit=%d want %d", w.CurrentLimit, want)
	}

	// Hit just below max clamps to max.
	w = &WaitStrategy{CurrentLimit: waitStrategyMaxSpin - 1}
	w.Wait(func() bool { return true }, func() {})
	if w.CurrentLimit != waitStrategyMaxSpin {
		t.Errorf("Wait hit clamp: CurrentLimit=%d want %d", w.CurrentLimit, waitStrategyMaxSpin)
	}

	// Miss: condition always false -> shrink by DecStep, sleepAction runs once.
	w = &WaitStrategy{CurrentLimit: 5000}
	slept := 0
	if w.Wait(func() bool { return false }, func() { slept++ }) {
		t.Fatal("Wait: expected not-ready on an always-false condition")
	}
	if slept != 1 {
		t.Errorf("Wait miss: sleepAction calls=%d want 1", slept)
	}
	if want := int32(5000) - waitStrategyDecStep; w.CurrentLimit != want {
		t.Errorf("Wait miss shrink: CurrentLimit=%d want %d", w.CurrentLimit, want)
	}

	// Miss just above min clamps to min.
	w = &WaitStrategy{CurrentLimit: waitStrategyMinSpin + 1}
	w.Wait(func() bool { return false }, func() {})
	if w.CurrentLimit != waitStrategyMinSpin {
		t.Errorf("Wait miss clamp: CurrentLimit=%d want %d", w.CurrentLimit, waitStrategyMinSpin)
	}
}

// TestWaitStateLimitAdaptation mirrors TestWaitLimitAdaptation for the
// assembly-driven WaitState path, which must adapt the limit identically.
func TestWaitStateLimitAdaptation(t *testing.T) {
	// Hit: *addr already equals want -> grow by IncStep.
	var v uint32 = 7
	w := &WaitStrategy{CurrentLimit: 1000}
	if !w.WaitState(&v, 7, func() { t.Fatal("sleepAction must not run on a hit") }) {
		t.Fatal("WaitState: expected ready when *addr == want")
	}
	if want := int32(1000) + waitStrategyIncStep; w.CurrentLimit != want {
		t.Errorf("WaitState hit grow: CurrentLimit=%d want %d", w.CurrentLimit, want)
	}

	// Miss: *addr never equals want -> shrink by DecStep, sleepAction runs once.
	v = 0
	w = &WaitStrategy{CurrentLimit: 5000}
	slept := 0
	if w.WaitState(&v, 7, func() { slept++ }) {
		t.Fatal("WaitState: expected not-ready when *addr != want")
	}
	if slept != 1 {
		t.Errorf("WaitState miss: sleepAction calls=%d want 1", slept)
	}
	if want := int32(5000) - waitStrategyDecStep; w.CurrentLimit != want {
		t.Errorf("WaitState miss shrink: CurrentLimit=%d want %d", w.CurrentLimit, want)
	}
}
