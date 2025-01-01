import esphome.codegen as cg
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
)
from esphome.const import (
    CONF_PORT,
    CONF_NAME,
    CONF_ID,
)
import esphome.config_validation as cv

from esphome.core import CORE

DEPENDENCIES = ["esp32"]

CONF_SAMPLE_INSERTION = "sample_insertion"
CONF_HOSTNAME = "hostname"

snapclient_ns = cg.esphome_ns.namespace("snapclient")
SnapClientComponent = snapclient_ns.class_("SnapClientComponent", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SnapClientComponent),
            cv.Optional(CONF_NAME): cv.string,
            cv.Optional(CONF_HOSTNAME, default=0): cv.domain,
            cv.Optional(CONF_PORT, default=1704): cv.port,
            cv.Optional(CONF_SAMPLE_INSERTION, default=True): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.require_framework_version(esp_idf=cv.Version(5, 1, 1)),
)


async def to_code(config):
    add_idf_component(
        name="espressif__esp-dsp",
        repo="https://github.com/espressif/esp-dsp.git",
    )
    add_idf_component(
        name="mdns",
        repo="https://github.com/espressif/esp-protocols.git",
        path="components/mdns",
    )
    add_idf_component(
        name="snapcast",
        repo="..",  # todo: replace by repo
        ref="esphome",
        # repo="https://github.com/CarlosDerSeher/snapclient.git",
        path="components/",
        components=[
            "lightsnapcast",
            "custom_board",
            "audio_hal",
            "audio_sal",
            "audio_board",
            "esp_peripherals",
            "libbuffer",
            "libmedian",
            "opus",
            "flac",
            "ui_http_server",
            "dsp_processor",
        ],
        submodules=[
            "components/opus/opus",
            "components/flac/flac",
        ],
    )
    cg.add_build_flag("-DCONFIG_WEB_PORT=8000")  # todo: make webserver optional
    add_idf_sdkconfig_option("CONFIG_USE_DSP_PROCESSOR", True)
    add_idf_sdkconfig_option("CONFIG_SNAPCLIENT_DSP_FLOW_STEREO", True)
    add_idf_sdkconfig_option("CONFIG_SNAPCLIENT_USE_SOFT_VOL", True)
    if CONF_NAME not in config:
        config[CONF_NAME] = CORE.name or ""
    cg.add_define("CONFIG_SNAPSERVER_HOST", config[CONF_HOSTNAME])
    cg.add_define("CONFIG_SNAPSERVER_PORT", config[CONF_PORT])
    if config[CONF_HOSTNAME] == 0:
        cg.add_define("CONFIG_SNAPCLIENT_USE_MDNS", False)
    else:
        cg.add_define("CONFIG_SNAPCLIENT_USE_MDNS", True)
    cg.add_define("CONFIG_SNAPCLIENT_NAME", config[CONF_NAME])
    cg.add_define("CONFIG_USE_SAMPLE_INSERTION", config[CONF_SAMPLE_INSERTION])

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
