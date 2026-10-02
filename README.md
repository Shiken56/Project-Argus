# Project Argus: Distributed Edge AI Multi-Camera Spatial Intelligence System

[![Platform](https://img.shields.io/badge/Platform-STM32N6570--DK-blue.svg)](https://www.st.com/en/evaluation-tools/stm32n6570-dk.html)
[![RTOS](https://img.shields.io/badge/RTOS-%CE%BCT--Kernel%203.0%20(TRON)-orange.svg)](https://www.tron.org/)
[![NPU Accelerator](https://img.shields.io/badge/NPU-Neural--ART%20Accelerator-green.svg)](https://www.st.com/)
[![Networking](https://img.shields.io/badge/Networking-LwIP%20UDP%20Gigabit-purple.svg)](https://savannah.nongnu.org/projects/lwip/)

---

## 1. Executive Summary & Real-World Significance

Traditional multi-camera video surveillance and analytics architectures suffer from two fundamental bottlenecks: **extreme network bandwidth consumption** and **centralized server compute saturation**. Streaming raw 1080p/4K video feeds from dozens or hundreds of cameras to a cloud server introduces severe latency, incurs massive cloud infrastructure costs, and introduces significant privacy vulnerabilities.

**Project Argus** overcomes these challenges by executing **distributed, real-time Edge AI vision pipelines directly on ultra-low-power microcontrollers**:
- **Zero Raw-Video Latency & Zero Cloud Dependent Compute**: Deep neural network inference is computed locally on the camera nodes in real time.
- **Privacy by Design**: Video processing happens entirely on-device; only processed visual chunks, bounding box detections, and high-dimensional feature embeddings are transmitted.
- **Low Power & Industrial Scalability**: Built on the STMicroelectronics STM32N6 series powered by an Arm Cortex-M55 core and the hardware **Neural-ART NPU**, delivering high-throughput inference at milliwatt power budgets.

### Key Capabilities
- **On-Device Object Detection**: Real-time person and object localization directly from MIPI-CSI2 sensor frames.
- **Deep Feature Extraction (ReID Embeddings)**: Running specialized feature extraction networks on the integrated NPU to compute distinct appearance descriptors for spatial multi-camera re-identification.
- **Hard Real-Time Determinism with μT-Kernel 3.0**: Leveraging the TRON Forum microT-Kernel 3.0 RTOS to guarantee zero frame dropping across preemptive camera capture (DCMIPP), NPU execution, and network transmission.

---

## 2. System Architecture

```
  +-------------------------------------------------------------------------------+
  |                        STM32N6570-DK Edge Vision Node                         |
  |                                                                               |
  |  +----------------+    MIPI-CSI2    +------------------+    AXI DMA           |
  |  |  Camera Module | --------------> |   CSI-2 Host     | -------------\       |
  |  |  (IMX335/OV56) |                 |    + DCMIPP      |              |       |
  |  +----------------+                 +------------------+              v       |
  |                                                                +------------+ |
  |  +----------------------------------------------------------+  | HyperRAM / | |
  |  |                 microT-Kernel 3.0 (TRON)                 |  | AXISRAM    | |
  |  |  - Sensor Acquisition Task                               |  +------------+ |
  |  |  - Deep Learning Pipeline:                               |         |       |
  |  |    * Mobile Object Detection                             |         |       |
  |  |    * ReID Feature Extraction (Neural-ART NPU)            | <-------/       |
  |  |  - LwIP Network Streaming & Telemetry Task               |                 |
  |  +----------------------------------------------------------+                 |
  |                               |                                               |
  +-------------------------------|-----------------------------------------------+
                                  | Ethernet (LwIP UDP)
                                  v
  +-------------------------------------------------------------------------------+
  |                        High-Speed Local Ethernet Switch                       |
  |            [Port 1: Board 1]      [Port 2: Board 2]      [Port 3: Host PC]    |
  +-------------------------------------------------------------------------------+
                                                             |
                                                             v
  +-------------------------------------------------------------------------------+
  |                 Central Aggregator: Project Argus Server                      |
  |  - Multi-Camera Frame Ingestion & High-Performance Rendering                  |
  |  - Real-Time Cross-Camera Feature Matching & Spatial Tracking Display         |
  +-------------------------------------------------------------------------------+
```

---

## 3. Hardware & Network Topology Setup

### Physical Connections
1. **STM32 Board 1**:
   - Connect the board's RJ45 Ethernet port to **Port 1** of the Ethernet switch.
   - Connect the board's ST-LINK USB-C port to the evaluation laptop.
2. **STM32 Board 2**:
   - Connect the board's RJ45 Ethernet port to **Port 2** of the Ethernet switch.
   - Connect the board's ST-LINK USB-C port to the evaluation laptop (or a second USB port).
3. **Evaluation Laptop (Host Server)**:
   - Connect the laptop's RJ45 Ethernet port (or USB-to-Ethernet adapter) to **Port 3** of the Ethernet switch.

### Laptop Static IP Configuration
The embedded boards communicate over the `192.168.1.0/24` subnet. Configure your laptop's Ethernet adapter with a static IP:

- **IP Address**: `192.168.1.100`
- **Subnet Mask**: `255.255.255.0`
- **Default Gateway**: `192.168.1.1` (or leave empty)

#### PowerShell Setup Command (Run as Administrator):
```powershell
# Identify your Ethernet interface name (e.g., "Ethernet" or "Ethernet 2")
Get-NetAdapter

# Apply the static IP (replace "Ethernet" with your adapter name if different)
New-NetIPAddress -InterfaceAlias "Ethernet" -IPAddress 192.168.1.100 -PrefixLength 24
```

---

## 4. Evaluation Guide (Step-by-Step for Judges)

### Step 1: Launch the Central Aggregator Server

The central tracking and multi-camera display server is hosted in its dedicated repository:
👉 [Project-Argus---Server Repository](https://github.com/Shishir-Hegde/Project-Argus---Server)

1. Clone or pull the latest server repository on the evaluation laptop:
   ```cmd
   git clone https://github.com/Shishir-Hegde/Project-Argus---Server.git
   cd Project-Argus---Server
   git pull origin main
   ```
2. Build and launch the server using the provided automation script:
   ```cmd
   build_and_run.bat
   ```
   *Alternatively, if running the precompiled executable directly:*
   ```cmd
   Project-Argus-Server.exe
   ```
3. The server application window will initialize and listen on UDP for incoming edge camera streams and telemetry vectors.

---

### Step 2: Configure STM32 Hardware Boot Switches (Debug Mode)

Before connecting the boards, ensure the boot switches are configured in **Development / Debug Mode**:
- Set both boot switch positions (**BOOT0** and **BOOT1**) to **0 / Development Boot Mode**.
- Connect the ST-LINK USB-C cable to power the board and enable JTAG/SWD debugging.

---

### Step 3: Open and Run Firmware via STM32CubeIDE

1. Clone the Project Argus firmware repository:
   ```cmd
   git clone https://github.com/Shiken56/Project-Argus.git
   cd Project-Argus
   git pull origin main
   ```
2. Launch **STM32CubeIDE** (version 2.0 or newer).
3. Select **File -> Open Projects from File System...**, browse to the cloned directory, and import the project.
4. You will see two sub-projects in the Project Explorer:
   - `mtk3bsp2_stm32n657_FSBL` (First Stage Bootloader)
   - `mtk3bsp2_stm32n657_Appli` (Application: TRON RTOS + Edge AI + LwIP)

> [!IMPORTANT]
> **Execution Order is Critical:**
> You **MUST** start debugging from **`mtk3bsp2_stm32n657_FSBL`**. 
> - The STM32N6 relies on the First Stage Bootloader to configure the Resource Isolation Framework (RIF / RIMC / RISC), clock distributions, and security attributes.
> - Once FSBL runs, it initializes system isolation and smoothly branches execution into the microT-Kernel application inside AXISRAM (`0x34000000`).

5. Right-click on **`mtk3bsp2_stm32n657_FSBL`** -> **Debug As** -> **STM32 C/C++ Application**.
6. When the debugger hits `main()` in FSBL, click **Resume (F8)**.
7. Repeat the launch for the second board connected via the second ST-LINK instance.

---

## 5. Verification & Runtime Output

1. **Serial Console Verification**:
   - Open a serial terminal (PuTTY / TeraTerm / Minicom) on the virtual COM port at **115200 baud, 8-N-1**.
   - You will observe microT-Kernel 3.0 initialization banners, Ethernet link negotiation, and Edge AI task activations:
     ```text
     [SYS] microT-Kernel 3.0 Booted Successfully
     [ETH] Link Up: 1000 Mbps Full Duplex
     [AI] Neural-ART NPU Initialized
     [AI] Detection & ReID Feature Pipeline Active
     [NET] Streaming Telemetry & Visual Stream to 192.168.1.100...
     ```
2. **Server Viewer Window**:
   - The server UI displays the live camera stream panels from the edge nodes.
   - Bounding boxes identify detected subjects.
   - Extracted ReID feature vectors are matched in real time to maintain cross-camera tracking identity.

---

## 6. Troubleshooting Checklist

| Issue | Root Cause | Solution |
| :--- | :--- | :--- |
| **No packets received on Server** | Laptop IP misconfigured | Verify laptop adapter is statically set to `192.168.1.100 / 24`. Run `ping 192.168.1.112`. |
| **Camera Warning / No Frame Received** | CSI flex ribbon cable loose | Unplug power, re-seat the MIPI-CSI camera ribbon cable firmly into the board connector, and verify latch is locked. |
| **Appli does not boot** | Run without FSBL | Ensure you launch the debug session targeting **`FSBL`** so system clocks and bus isolation are configured first. |
| **ST-LINK not detected** | ST-LINK USB driver missing | Install the ST-LINK USB driver included with STM32CubeIDE / STM32CubeProgrammer. |

---

## 7. Project Repositories & Resources

- **Firmware Repository**: [https://github.com/Shiken56/Project-Argus](https://github.com/Shiken56/Project-Argus)
- **Aggregator Server Repository**: [https://github.com/Shishir-Hegde/Project-Argus---Server](https://github.com/Shishir-Hegde/Project-Argus---Server)
- **TRON Forum Specifications**: [https://www.tron.org/](https://www.tron.org/)
