"""HA MQTT discovery and bidirectional master-volume state. No HA restart required."""
import json
import os
import threading
import time
import urllib.request
import paho.mqtt.client as mqtt

BASE = "teedsp/master"


def request(patch=None):
    req = urllib.request.Request("http://127.0.0.1:8080/api/volume",
        data=None if patch is None else json.dumps(patch).encode(),
        headers={"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=3))


def start():
    if not os.environ.get("MQTT_PASSWORD"):
        return

    def worker():
        client = mqtt.Client(client_id="teedsp-master")
        client.username_pw_set(os.environ["MQTT_USER"], os.environ["MQTT_PASSWORD"])
        client.will_set(BASE+"/available", "offline", qos=1, retain=True)

        def publish_state(state):
            client.publish(BASE+"/volume", str(round(state["volume"])), qos=1, retain=True)
            client.publish(BASE+"/mute", "ON" if state["muted"] else "OFF", qos=1, retain=True)

        def connected(c, *_):
            device={"identifiers":["teedsp_cm3588"], "name":"TeeDSP", "manufacturer":"TeeDSP", "model":"CM3588 DSP"}
            for component, field, name in (("number","volume","Master volume"),("switch","mute","Mute")):
                config={"name":name,"unique_id":"teedsp_master_"+field,"object_id":"teedsp_master_"+field,
                    "device":device,"command_topic":BASE+"/"+field+"/set","state_topic":BASE+"/"+field,
                    "availability_topic":BASE+"/available"}
                if field=="volume": config.update({"min":0,"max":100,"step":1,"mode":"slider","unit_of_measurement":"%"})
                c.publish("homeassistant/"+component+"/teedsp_master_"+field+"/config",json.dumps(config),qos=1,retain=True)
            c.subscribe(BASE+"/+/set",qos=1)

        def message(c, _, msg):
            try:
                value=msg.payload.decode()
                if msg.topic.endswith("/volume/set"):
                    patch={"volume":float(value)}
                else:
                    if value not in ("ON","OFF"): return
                    patch={"muted":value=="ON"}
                publish_state(request(patch))
            except Exception as error:
                print("Volume command failed:",type(error).__name__,flush=True)

        client.on_connect=connected;client.on_message=message
        client.connect_async(os.environ.get("MQTT_HOST","192.168.1.218"),1883,30)
        client.loop_start()
        previous=None
        while True:
            try:
                state=request()
                if state!=previous or int(time.time())%15==0:
                    publish_state(state);previous=state
                client.publish(BASE+"/available","online",retain=True)
            except Exception:
                client.publish(BASE+"/available","offline",retain=True)
            time.sleep(1)
    threading.Thread(target=worker,daemon=True).start()
