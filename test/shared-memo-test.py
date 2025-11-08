import sys
sys.path.append("../scripts")
import numpy as np
from multiprocessing import shared_memory
from multiprocessing import Process,Lock,Event
import fastdb4py as fastdb
import time

handle_name = "my_shared_memoryx"
def process_task(name,event):
    event.wait()
    print("start subprocess....")
    #连接到已存在的共享内存块
    shm = shared_memory.SharedMemory(name=name)
    db = fastdb.WxDatabase.load_xbuffer(shm.buf)
    print("read in subprocess with shared memo")
    for i in range(db.get_layer_count()):
        layer = db.get_layer(i)
        print("Layer:", layer.name())
        print("Feature count:", layer.get_feature_count())
        print("Field count:", layer.get_field_count())
        print("Field definitions:")
        for i in range(layer.get_field_count()):
            print("  ", layer.get_field_defn(i))

    shm.close()
    event.set()

if __name__ == "__main__":
    event = Event()
    data = open("./wx-demo.db", "rb").read()
    shm_a = shared_memory.SharedMemory(create=True, size=len(data), name=handle_name)
    dest = np.ndarray((len(data),),dtype=np.uint8,buffer=shm_a.buf)
    dest[:] = np.frombuffer(data,dtype=np.uint8)
    print("数据已复制到共享内存中。")
    try:
        p = Process(target=process_task, args=(handle_name,event))
        p.start()
        event.set()
        p.join()
    finally:
        time.sleep(1)
        event.wait()
        print("数据已从共享内存中删除。")
        shm_a.close()
        shm_a.unlink()