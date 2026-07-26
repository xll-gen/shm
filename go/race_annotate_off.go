//go:build !race

package shm

// Production build: the slot-ownership handoff annotations are compiled away
// entirely. See race_annotate_on.go for what they do and why they exist.

func releaseSlotHandoff(*slotContext) {}

func acquireSlotHandoff(*slotContext) {}
