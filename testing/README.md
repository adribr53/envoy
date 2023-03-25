- Build Envoy: ./ci/run_envoy_docker.sh './ci/do_ci.sh bazel.release.server_only'

- Run Sender proxy with debug mode: /tmp/envoy-docker-build/envoy/source/exe/envoy/envoy -l debug -c sender_config1.yaml
- Run Receiver proxy with debug mode: /tmp/envoy-docker-build/envoy/source/exe/envoy/envoy -l debug -c receiver_config1.yaml --base-id 1

- Run a server that echoes back every message received: ncat -e /bin/cat -k -l 3000
- Run a TCP client that sends a file to the server through both proxies: python3 client.py

- It is important to run the server before the client or this one won't be able to join the server through the custom socket
  If they are not run in this order, a restart (and sometimes kill) of the proxies will be needed (because they will be blocked)

- Interesting files:
    - Custom sender and receiver filters are located in /envoy/source/extensions/filters/network/sender and /envoy/source/extensions/filters/network/receiver respectively. Files: sender.cc, sender.h, receiver.cc, receiver.h

- Current flaws:
    - The current implementation does not perform well with many successive small client connections. For instance, using as destination "cluster_nginx"
    on the Receiver proxy and sending HTTP requests with "./wrk http://localhost:1001 -c 1 -t 1 -d 10 -L" produces strange results
    and the proxies do not react well to that.
    - The implementation works for a single TCP client connection (for instance using client.py) but I am not sure it works well
    for large data transfer (for instance using large_file.txt), there are cases where the data sent by the client is not completely received back 
    and threads of both proxies are stuck.
    - Circular buffers currently store string representations of the Buffer InstancePtr of Envoy API, there may be performance loss by converting
    them to a string (using toString()) each time a new buffer appears. Moreover, it may be better to allocate buffers on the heap
    and I also need to check for potential memory leaks.

- Performance testing:
    - I used "iperf -s -p 3000" for the server and "iperf -c localhost -p 1001 -t 10 -P 1 -l 100" as the client to check for performance and it seems to work with a single connection. But when adding "-d" parameter to allow bidirectionnal communication, there are sometimes a bug in the Receiver proxy which cause it to not detect connection close. Increasing the -l parameter also gives worse results than TCP proxy, but for values equal or below 100, it seems reasonable.
    - I made comparison with TCP using "tcp1_config1.yaml" as downstream proxy and "tcp2_config1.yaml" as upstream proxy
    - I tested the correctness of the requests/responses with a custom TCP client in Python (client.py) and for the server "ncat -e /bin/cat -k -l 3000",
    it is basically a file transfer that is written back to the client. There are probably better ways to test the correctness. As I said, for large files, it sometimes does not work as expected.