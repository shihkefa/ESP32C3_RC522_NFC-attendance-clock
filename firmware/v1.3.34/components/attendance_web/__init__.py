import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light, time, wifi
from esphome.const import CONF_ID

DEPENDENCIES = ["web_server_base", "time", "light", "wifi"]

attendance_ns = cg.esphome_ns.namespace("attendance_web")
AttendanceWeb = attendance_ns.class_("AttendanceWeb", cg.Component)
CONF_TIME_ID = "time_id"
CONF_HARDWARE_RTC_ID = "hardware_rtc_id"
CONF_STATUS_LIGHT_ID = "status_light_id"
CONF_WIFI_ID = "wifi_id"
CONF_ADMIN_PASSWORD = "admin_password"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AttendanceWeb),
    cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    cv.Required(CONF_HARDWARE_RTC_ID): cv.use_id(time.RealTimeClock),
    cv.Required(CONF_STATUS_LIGHT_ID): cv.use_id(light.LightState),
    cv.Required(CONF_WIFI_ID): cv.use_id(wifi.WiFiComponent),
    cv.Required(CONF_ADMIN_PASSWORD): cv.string_strict,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    rtc = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(rtc))
    hardware_rtc = await cg.get_variable(config[CONF_HARDWARE_RTC_ID])
    cg.add(var.set_hardware_rtc(hardware_rtc))
    status_light = await cg.get_variable(config[CONF_STATUS_LIGHT_ID])
    cg.add(var.set_status_light(status_light))
    wifi_component = await cg.get_variable(config[CONF_WIFI_ID])
    cg.add(var.set_wifi(wifi_component))
    cg.add(var.set_admin_password(config[CONF_ADMIN_PASSWORD]))
