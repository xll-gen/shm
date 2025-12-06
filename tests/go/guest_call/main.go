package main

import (
    "fmt"
    "time"
    "github.com/xll-gen/shm/go"
)

func main() {
    // Wait for Host to initialize
    time.Sleep(1 * time.Second)

    fmt.Println("Connecting to GuestCallTest...")
    client, err := shm.Connect("GuestCallTest")
    if err != nil {
        panic(err)
    }
    defer client.Close()

    // Start workers (even if we don't handle host requests, we need to map slots)
    // Actually Connect calls Start? No, user calls Start.
    // For Guest Call, we don't strictly need to start workers if we only send.
    // But Connect connects to the SHM.

    // Send Guest Call
    fmt.Println("Sending Guest Call...")
    resp, err := client.SendGuestCall([]byte("PING"), shm.MsgTypeGuestCall)
    if err != nil {
        panic(err)
    }

    fmt.Printf("Received Response: %s\n", string(resp))
    if string(resp) != "ACK" {
        panic("Response mismatch")
    }
    fmt.Println("Test Passed.")
}
