Change the channel from sensor to controller to **FIFO** and added the ```HMAC integrity tag``` to the message.

- pass: sensor to FIFO ```y.pipe``` to controller (without MITM). Controller verifies ```HMAC: mac_ok=1```, means that the transfer information is not be modified.

- bias: sensor to FIFO ```y_in.pipe``` to MITM(+bias) to FIFO ```y_out.pipe``` to controller. MITM must additionally perform ```open/read/write```. MITM changed the value but did not update HMAC → controller verification failed: ```mac_ok=0```.
