from __future__ import annotations

from typing import Literal

from esphome import pins
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_FLOW_CONTROL_PIN, CONF_UART_ID
from esphome.cpp_helpers import gpio_pin_expression

AUTO_LOAD = []
DEPENDENCIES = ["uart"]
CODEOWNERS = ["@nspitko"]

broan_ns = cg.esphome_ns.namespace("broan")
BroanComponent = broan_ns.class_("BroanComponent", cg.Component, uart.UARTDevice)

CONF_BROAN_ID = "broan_id"
CONF_REMOTE_UART_ID = "remote_uart_id"
CONF_REMOTE_FLOW_CONTROL_PIN = "remote_flow_control_pin"
CONF_UART_DIAGNOSTIC = "uart_diagnostic"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BroanComponent),
            cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_REMOTE_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_REMOTE_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_UART_DIAGNOSTIC, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

def final_validate_schema(config):
    config = uart.final_validate_device_schema(
        "broan",
        require_tx=True,
        require_rx=True,
        parity="NONE",
        stop_bits=1,
    )(config)

    if CONF_REMOTE_UART_ID not in config:
        if CONF_REMOTE_FLOW_CONTROL_PIN in config:
            raise cv.Invalid("remote_flow_control_pin requires remote_uart_id")
        return config

    if config[CONF_UART_ID] == config[CONF_REMOTE_UART_ID]:
        raise cv.Invalid("remote_uart_id must reference a different UART than uart_id")

    return uart.final_validate_device_schema(
        "broan remote",
        uart_bus=CONF_REMOTE_UART_ID,
        require_tx=True,
        require_rx=True,
        parity="NONE",
        stop_bits=1,
    )(config)

BroanBaseSchema = cv.Schema(
    {
        cv.GenerateID(CONF_BROAN_ID): cv.use_id(BroanComponent),
    },
)

FINAL_VALIDATE_SCHEMA = final_validate_schema

async def to_code(config):
    cg.add_global(broan_ns.using)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    await uart.register_uart_device(var, config)

    if CONF_FLOW_CONTROL_PIN in config:
        pin = await gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))

    if CONF_REMOTE_UART_ID in config:
        remote_uart = await cg.get_variable(config[CONF_REMOTE_UART_ID])
        cg.add(var.set_remote_uart_parent(remote_uart))

    if CONF_REMOTE_FLOW_CONTROL_PIN in config:
        pin = await gpio_pin_expression(config[CONF_REMOTE_FLOW_CONTROL_PIN])
        cg.add(var.set_remote_flow_control_pin(pin))

    cg.add(var.set_uart_diagnostic_mode(config[CONF_UART_DIAGNOSTIC]))
