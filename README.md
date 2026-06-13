# Broan ERV serial component

An ESP32 component to communicate with Broan, Nutone, Venmar, and VanEE ERVs via their rs485 interface.

The protocol is documented [here](https://spitko.net/2025/08/08/Reverse-Engineering-an-ERV/)

Currently this project is stable, but may be missing some advanced features from certain models. If there is a feature you'd like added, please open an issue. We may need packet capture from a unit with this functionality.

<table>
  <tr>
    <td>
      <img width="332" height="477" alt="image" src="https://github.com/user-attachments/assets/5170773c-7def-4df2-bef0-9fa33ca78082" />
    </td>
    <td valign="top">
      <img width="329" height="367" alt="image" src="https://github.com/user-attachments/assets/2e4aba9a-a7a8-4349-9e79-a22f0018b23a" />
    </td>
  </tr>
</table>

## Supported models
We currently don't have a good range of what does and don't work, but it's currently believed that all Broan, Nutone, and VanEE ERVs that have rs485 inputs probably work. The easiest way to check is to see if your unit supports the VTTOUCHW (All three brands have a version of this interface, they are all presumed to be the same)

HRVs have been reported to work as well, but tend to support fewer features. You can safely remove sensors from the yaml that your device does not support.

## Requirements
1) You will need an esphome device that can communicate over rs485. Some devices come with this out of the box (waveshare esp32-s3-relay-6ch), or you can just buy an external tranceiver and wire it to the uart of your choice. Note that the waveshare device listed earlier is much easier to use than most, as it has automatic flow control and can be powered directly from the 12v output on your ERV. If you use a different device and make a working, stable configuration, please open an issue so we can start building a list with configuration files.
2) In the default single-port configuration this library replaces the serial wall remote. To keep the wall remote installed, use pass-through mode with a second RS485 transceiver.
3) You will ideally want to power this directly from the 12v output on the erv itself. this isn't a hard requirement, but it simplifies things a lot. If the ERV completes the handshake with the esp32 and it later goes away, the ERV will eventually drop into an error state and shut down, so it's just one less point of failure.

## Installation
Near the control interface on the ERV, look for a green terminal block that has D+, D-, and GND on it (typically a 6 terminal block that includes 12V, LED, and OVR). Connect the D+, D-, and GND connectors to your RS485 tranceiver. Ideally you can also use the +12v but very few ESP32s are set up to handle input voltages above 5V to check with your spec sheet first. If not, just use a USB cable and wall charger. If you go this route, you may need to additionally run a wire from the GND terminal on the ERV to the GND pin on the esp32, but this will depend on how your rs485 tranceiver is set up. Only do this if you're getting unexplained communication errors or crashes.

Some rs485 trancevers have a jumper for the terminating resistor, some do not. In my case I found I did not need a termination resister, but you may need one. Try enabling this, or adding a resistor manually across the terminals on the ESP32 side, if you have communication issues. You'll know if you flip these because you'll see a bunch of spew about alignment errors and unknown commands.

Also be aware some RS485 devices will label their pins A and B instead of D+ and D-. Somewhat confusingly, A is D- and B is D+

## Supported features
* Setting fan mode (Standby, Min, Max, Intermittent, Turbo, Override, and Med, which is treated as manual control)
* Setting fan speed in manual mode
* Humidity control mode
* Intake temperature
* Filter life left
* Fan CFM 

More features will be added as time allows. I've documented many fields that aren't supported yet. If there's a specific feature you want prioritized, open an issue. This project is at a point where it "works for me" so I don't have a lot of guiding light on what else should be added without external input.

## Humidity Control Mode
In Humidity Control Mode, the controller sets a target humidity level and the ERV automatically runs if the humidity is above this level. The ERV does not have a humidity sensor - the current humidity reading is sent periodically from the controller. In single-port mode, the ESP device replaces the original controller and must send current humidity readings itself. This can be done from a lambda in your ESPHome config calling the setCurrentHumidity() function. For an example configuration, see the [humidity_control_sample.yaml](./examples/humidity_control_example.yaml) configuration in the [examples](./examples/) directory.

To use humidity control mode once it is enabled, set the desired humidity with "Humidity Setpoint", and then turn on the Humidity Control switch.

## Pass-through wall remote mode
Pass-through mode lets the ESP32 sit between the ERV/HRV and the original serial wall remote. Configure `uart_id` as the HRV-side RS485 port and `remote_uart_id` as the wall-remote-side RS485 port. Frames from either side are forwarded unchanged, so the wall remote continues to work and display current state.

When Home Assistant queues a command, the ESP32 takes one private HRV control grant, sends its command, receives the reply, then releases control and resumes transparent forwarding. If your RS485 boards need manual driver-enable pins, set `flow_control_pin` for the HRV side and `remote_flow_control_pin` for the wall remote side.

The HRV-side and wall-remote-side A/B wires must be two separate RS485 buses; do not tie the two local transceiver A/B pairs together. Keep grounds common. Terminate each bus at its physical ends. Long wall-remote runs may need a 120 ohm resistor across A/B at the ESP remote-side terminal or transceiver footprint, especially if the remote receives forwarded pings but does not answer. With power off, a correctly terminated segment with two 120 ohm terminators usually measures about 60-70 ohm across A/B; about 120-160 ohm indicates one terminator, and much less than 60 ohm suggests too much termination.

Some units use a client address other than the default `0x12`. Use `uart_diagnostic` with both HRV and wall remote connected to confirm the address, then set the reported `client_address` under `broan:`. The server address defaults to `0x10`.

See [examples/pass_through_example.yaml](./examples/pass_through_example.yaml) for a two-UART configuration.

## UART diagnostic mode
Use UART diagnostic mode when wiring a new RS485 adapter, checking A/B polarity, or finding the client address used by a specific ERV/HRV. Diagnostic mode disables normal control while it is active.

Add `uart_diagnostic: true` under `broan:` and keep the UART pins you want to test:

```yaml
uart:
  - id: hrv_rs485
    tx_pin: GPIO17
    rx_pin: GPIO18
    baud_rate: 38400
    rx_buffer_size: 2048

  - id: remote_rs485
    tx_pin: GPIO25
    rx_pin: GPIO26
    baud_rate: 38400
    rx_buffer_size: 2048

broan:
  id: mybroan
  uart_id: hrv_rs485
  remote_uart_id: remote_rs485
  uart_diagnostic: true
```

After boot, the diagnostic waits 30 seconds, then cycles common baud rates (`9600`, `19200`, `38400`, `57600`, `115200`) with `inverted=false` and `inverted=true`. Each attempt runs for 10 seconds and logs whether data was received, raw byte bursts in hex, invalid frame counts, and any valid Broan frames.

When one side receives a valid Broan frame, the diagnostic pins that baud/inversion setting, applies the same UART config to the other side, and starts forwarding valid frames between the HRV and remote UARTs. It stops only after both sides have received valid frames, traffic has been forwarded in both directions, and a remote-side frame has confirmed `client_address`.

The HRV may send `Ping` frames while sweeping through possible target addresses. Those targets are reported as probe targets, not as confirmed client addresses. If only the HRV side is connected, the diagnostic will keep forwarding HRV frames and reporting that it is waiting for a remote-side response.

When diagnostic mode reports success, copy the reported settings into the normal config and remove `uart_diagnostic: true`:

```yaml
uart:
  - id: hrv_rs485
    tx_pin:
      number: GPIO17
      inverted: false
    rx_pin:
      number: GPIO18
      inverted: false
    baud_rate: 38400
    rx_buffer_size: 2048

  - id: remote_rs485
    tx_pin:
      number: GPIO25
      inverted: false
    rx_pin:
      number: GPIO26
      inverted: false
    baud_rate: 38400
    rx_buffer_size: 2048

broan:
  id: mybroan
  uart_id: hrv_rs485
  remote_uart_id: remote_rs485
  # Use the confirmed value printed by diagnostic success.
  client_address: 0x01
```

## FAQ
Q: I see errors about failed communication

A: This could be a lot of things.
- If you're getting timeouts, it's probably the yaml being misconfigured. A lot of rs485 devices want a flow control pin, which needs to be specified (check your device's datasheet)
- If you see it getting data but complaining about alignment and unknown commands, it's likely either you swapped the +/- wires, or the communication is very weak. Double check that you wired up the ground wire correctly, and try adding or removing ESP-side termination.

Q: Why doesn't it support X?

A: Not everything is actually exposed via the rs485 interface. I may need dumps from a unit with the feature you're requesting, or it might already be mapped and just needs to be plumbed though to Home Assistant. Feel free to open an issue or PR

Q: What if I still want wall controls?

A: Use pass-through mode with two RS485 ports if you want to keep the serial wall remote. Aux remotes using the dry-contact interface also work as hard overrides; the fan mode will indicate "ovr" (override) when these controls are used.

### ESPhome yaml
Add this to an existing config.
```
external_components:
  - source:
      type: git
      url: https://github.com/nspitko/broan_erv_uart
      ref: main
    components: [ broan ]

uart:
  id: rs485
  tx_pin: GPIO17 # Change these to match your rs485 tranceiver. 17/18 is txd1/rxd1
  rx_pin: GPIO18
  baud_rate: 38400
  rx_buffer_size: 2048
  #debug:
   # direction: BOTH
   # dummy_receiver: false
   # sequence:
   #   - lambda: UARTDebug::log_hex(direction, bytes, ' ');

broan:
  uart_id: rs485

select:
  - platform: broan
    fan_mode:
      name: "fan mode"

number:
  - platform: broan

    # Speed as a ratio of min vs max speed. Eg, if your MIN is 20 CFM and your MAX is 
    # 40 CFM, 50% means medium rill be at 30 CFM
    fan_speed:
      name: "fan speed"

    # How many seconds ON per hour in int mode (eg, a value of 20 means the ERV will run
    # 20 minutes on 40 minutes off every hour )
    intermittent_period:
      name: "Intermittent Period"

sensor:
  - platform: broan
    # As reported by the ERV, in watts
    power:
      name: Power draw

    # Intake air temperature. This will generally read high
    temperature:
      name: Temperature

    # In Days
    filter_life:
      name: Remaining Filter life

    # CFM values as reported by the ERV
    supply_fan_cfm:
      name: "Supply fan CFM"
    exhaust_fan_cfm:
      name: "Exhaust fan CFM"

    # Fan motor RPM values
    supply_fan_rpm:
      name: "Supply fan RPM"
    exhaust_fan_rpm:
      name: "Exhaust fan RPM"

    # Not all ERVs have these sensor, remove if you don't get readings from it.

    # Exhaust air temperature.
    temperature_out:
      name: Temperature Out

button:
  - platform: broan

    # Resets filter life to 7884000 seconds / 3 months (Default behavior)
    filter_reset:
      name: Filter reset

```
