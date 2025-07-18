# start with this as it is tested.
# later on you can play/investigate the idf_component.yml 
# which seems more uptodate, but never tested

idf_component_register(
	SRCS "app_main.cpp" 
		 "wifi.c" 
		 "websocket_client.cpp"
		 "face_sender.cpp"
		 "message_handler.cpp"
		 "heartbeat.cpp"
		 "face_sender.cpp"
		 "time_sync.c"
		 
	INCLUDE_DIRS "."
	REQUIRES 
			json
			esp_websocket_client
			esp_psram
			esp32-camera
	PRIV_REQUIRES 
			modules # who_camera.h, who_human_face_detection.hpp
                      )
