# SafePulse Lab: ISO 26262 Goal Structuring Notation (GSN) Safety Case

This work product defines the formal argument and evidence structure demonstrating that **SafePulse Lab** satisfies its ASIL-D safety goals when operating as an automotive safety gateway on BlackBerry QNX Neutrino 8.0.

---

## 1. Top-Level Safety Architecture (GSN Hierarchy)

```mermaid
graph TD
    G1["G1: The SafePulse Gateway maintains vehicle safe state across all operational and fault conditions"]
    
    S1["S1: Argument by decomposition into Top-Level Safety Goals (SG-01, SG-02, SG-03)"]
    G1 --> S1
    
    G_SG01["G_SG01 (SG-01): Freedom from Unintended Vehicle Motion & Plausibility Contradiction"]
    G_SG02["G_SG02 (SG-02): Deterministic Safe State Transition within 10ms FTTI"]
    G_SG03["G_SG03 (SG-03): High-Availability Microkernel Recovery & Fault Containment"]
    
    S1 --> G_SG01
    S1 --> G_SG02
    S1 --> G_SG03

    %% SG-01 Decomposition
    S_SG01["S_SG01: Argument on Multi-Layer Plausibility & Closed-Loop Actuator Clamping"]
    G_SG01 --> S_SG01
    E_Linter["E1: Inline Real-Time Safety Linter (Rules 0x1001-0x1008)"]
    E_Slew["E2: Slew-Rate Limiter (250 deg/s steer, 25 km/h/s estop brake)"]
    E_Plant["E3: 100Hz Closed-Loop Vehicle Plant Physics Model (host/hil/vehicle_plant.py)"]
    S_SG01 --> E_Linter
    S_SG01 --> E_Slew
    S_SG01 --> E_Plant

    %% SG-02 Decomposition
    S_SG02["S_SG02: Argument on Real-Time Determinism & Core Affinity"]
    G_SG02 --> S_SG02
    E_WCET["E4: 100k-Sample Empirical WCET (207.9 µs vs 10,000 µs FTTI, 2.08% budget)"]
    E_FIFO["E5: SCHED_FIFO Pri 55 + ThreadCtl Core-0 Pinning + mlockall Paging Lock"]
    E_MCDC["E6: 100% MC/DC Test Suite on 4 Core Decision Branches (guest/test_mcdc_matrix.c)"]
    S_SG02 --> E_WCET
    S_SG02 --> E_FIFO
    S_SG02 --> E_MCDC

    %% SG-03 Decomposition
    S_SG03["S_SG03: Argument on Microkernel Process Isolation & Resilient Supervisor"]
    G_SG03 --> S_SG03
    E_Secpol["E7: QNX secpol Type-ID Kernel Channel Enforcement (safepulse_steering_t, etc.)"]
    E_Watcher["E8: High-Availability Watcher Daemon (Pri 55) with 300ms Deadlock Hang Detector"]
    E_Crash["E9: 50-Crash Multi-Fault Marathon with 100% Shared Memory State Retention"]
    E_CAN["E10: CAN FD Ingress Pipeline with 128x11 Recessive Bus-Off Recovery"]
    S_SG03 --> E_Secpol
    S_SG03 --> E_Watcher
    S_SG03 --> E_Crash
    S_SG03 --> E_CAN
```

---

## 2. Safety Goals and Traceability Matrix

| Safety Goal | ASIL | Description | Safety Mechanism | Empirical Evidence |
|---|---|---|---|---|
| **SG-01** | ASIL-D | Prevent erroneous acceleration or wild steering exceeding dynamic vehicle stability limits. | Inline Linter Engine + Dual-Rail FSM + Slew Clamp | [vehicle_plant.py](file:///C:/AI/projects/qnx-safepulse-lab/host/hil/vehicle_plant.py) (Stopping distance 16.4m, 100% PASS) |
| **SG-02** | ASIL-D | Complete fault detection, latching, and actuator safe-state command within 10ms FTTI. | Core 0 Affinity + `mlockall` + `SCHED_FIFO 55` | [benchmark_wcet.c](file:///C:/AI/projects/qnx-safepulse-lab/guest/benchmark_wcet.c) (WCET: 207.9 µs = 2.08% FTTI) |
| **SG-03** | ASIL-D | Maintain telemetry state, channel isolation, and service recovery without OS reboot upon process fault. | QNX secpol Type-ID + POSIX SHM Ringbuffer + Watcher | [safepulse_watcher.c](file:///C:/AI/projects/qnx-safepulse-lab/guest/safepulse_watcher.c) (40.5ms respawn, 10/10 BIST PASS) |

---

## 3. Scope & Verification Boundaries (Engineering Disclosures)

Per strict automotive safety auditing standards (ISO 26262-2:2018 Clause 6):
1. **Target Environment**: This project is developed and verified in a deterministic virtual prototype environment (**BlackBerry QNX Neutrino 8.0 on QEMU x86_64** with VirtIO network and Bosch M_CAN/FlexCAN HAL emulation).
2. **Physical Boundaries**: Physical bus transceivers, electrical termination, and actual silicon hardware (e.g. NXP S32G / Qualcomm SA8295P) are emulated through deterministic Software-in-the-Loop (SIL) and Hardware-in-the-Loop (HIL) bench simulators.
3. **Certification Status**: This repository represents an **audit-ready ASIL-D architectural reference prototype** demonstrating compliance with ISO 26262 Part 5 (FMEDA) and Part 6 (MC/DC & Software Architecture), not a commercial production certificate issued by a third-party registrar.
