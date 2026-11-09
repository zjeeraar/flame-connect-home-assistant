import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, number, select, switch
from esphome.const import (
    CONF_ID,
    CONF_RESTORE_MODE,
    DEVICE_CLASS_TEMPERATURE,
    UNIT_CELSIUS,
    UNIT_HOUR,
)

DEPENDENCIES = ["esp32", "ble_client"]

AUTO_LOAD = ["switch", "number", "select"]

flame_ns = cg.esphome_ns.namespace("flame")
Flame = flame_ns.class_("Flame", cg.Component)
FlameSwitch = flame_ns.class_("FlameSwitch", switch.Switch)
FlameNumber = flame_ns.class_("FlameNumber", number.Number)
FlameSelect = flame_ns.class_("FlameSelect", select.Select)

CONF_SWITCH = "toggle"
CONF_HEAT = "heat"
CONF_TEMPERATURE = "temperature"
CONF_VOLUME = "volume"
CONF_AMBIENT = "ambient"
CONF_BRIGHTNESS = "brightness"
CONF_TIMER = "timer"
CONF_SENSOR = "sensor"


def switch_schema():
    return switch.switch_schema(FlameSwitch).extend(
        {
            cv.Optional(CONF_RESTORE_MODE, default="RESTORE_DEFAULT_OFF"): cv.enum(
                switch.RESTORE_MODES
            )
        }
    )


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Flame),
            cv.Required(CONF_SWITCH): switch_schema(),
            cv.Required(CONF_HEAT): switch_schema(),
            cv.Required(CONF_TEMPERATURE): number.number_schema(
                FlameNumber,
                unit_of_measurement=UNIT_CELSIUS,
                device_class=DEVICE_CLASS_TEMPERATURE,
                icon="mdi:thermometer",
            ),
            cv.Required(CONF_VOLUME): number.number_schema(
                FlameNumber,
                icon="mdi:volume-high",
            ),
            cv.Required(CONF_AMBIENT): number.number_schema(
                FlameNumber,
                icon="mdi:palette",
            ),
            cv.Required(CONF_BRIGHTNESS): number.number_schema(
                FlameNumber,
                icon="mdi:fire",
            ),
            cv.Required(CONF_TIMER): number.number_schema(
                FlameNumber,
                unit_of_measurement=UNIT_HOUR,
                icon="mdi:timer-outline",
            ),
            cv.Required(CONF_SENSOR): select.select_schema(
                FlameSelect,
                icon="mdi:motion-sensor",
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    toggle = await switch.new_switch(config[CONF_SWITCH])
    heat = await switch.new_switch(config[CONF_HEAT])
    temperature = await number.new_number(
        config[CONF_TEMPERATURE], min_value=15, max_value=32, step=1
    )
    var = cg.new_Pvariable(config[CONF_ID], toggle, heat, temperature)

    volume = await number.new_number(
        config[CONF_VOLUME], min_value=0, max_value=6, step=1
    )
    ambient = await number.new_number(
        config[CONF_AMBIENT], min_value=1, max_value=8, step=1
    )
    brightness = await number.new_number(
        config[CONF_BRIGHTNESS], min_value=1, max_value=6, step=1
    )
    timer = await number.new_number(
        config[CONF_TIMER], min_value=0, max_value=8, step=0.5
    )
    sensor = await select.new_select(config[CONF_SENSOR], options=["Off", "Min", "Max"])

    cg.add(var.set_volume(volume))
    cg.add(var.set_ambient(ambient))
    cg.add(var.set_brightness(brightness))
    cg.add(var.set_timer(timer))
    cg.add(var.set_sensor(sensor))

    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
