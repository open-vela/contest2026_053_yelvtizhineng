if(CONFIG_SOIL_NODE)
  nuttx_add_application(
    NAME soil_node
    SRCS soil_node_main.c soil_config.c soil_http.c soil_mqtt.c soil_power.c
         soil_sensor.c soil_state.c soil_time.c soil_wifi.c
    STACKSIZE ${CONFIG_SOIL_NODE_STACKSIZE}
    PRIORITY ${CONFIG_SOIL_NODE_PRIORITY}
  )
endif()
