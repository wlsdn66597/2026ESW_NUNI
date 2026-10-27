import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID
DEPENDENCIES = ["uart", "logger"]
ns = cg.esphome_ns.namespace("nuni_mr60_serial")
SerialRadar = ns.class_("SerialRadar", cg.Component, uart.UARTDevice)
CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(SerialRadar)}).extend(uart.UART_DEVICE_SCHEMA).extend(cv.COMPONENT_SCHEMA)
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema("nuni_mr60_serial", require_tx=True, require_rx=True, baud_rate=115200, parity="NONE", stop_bits=1)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
