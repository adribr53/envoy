import socket
import threading
import os
import time

# Define the IP address and port number of the server
SERVER_IP = '127.0.0.1'
SERVER_PORT = 1001

test_file = 'medium_file.txt' # Change filename here (small_file.txt, medium_file.txt, large_file.txt)

file_size = os.path.getsize(test_file)
print(f"The size of {test_file} is {file_size} bytes.")

# Function to receive data from the server
def receive_data(sock):
    # Receive file from server
    with open('output.txt', 'wb') as f:
        print('Receiving data...')
        total = 0
        while True:
            try:
                data = sock.recv(64)
                # print(len(data))
                total += len(data)
                f.write(data)
                if total == file_size:
                    break;
            except TimeoutError:
                print('Not receiving any data after 5 seconds')
                print(total)
                return
        print(total)
        print('File received successfully.')

# Create a TCP socket and connect to the server
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(5)
sock.connect((SERVER_IP, SERVER_PORT))

# Start a thread to receive data from the server
recv_thread = threading.Thread(target=receive_data, args=(sock,))
recv_thread.start()

# open the file to be sent to the server
with open(test_file, 'rb') as f:
    # send the file to the server
    print('Sending data...')
    while True:
        chunk = f.read(64)
        if not chunk:
            break
        sock.sendall(chunk)
    print('File sent successfully.')

# Wait for the receive thread to finish
recv_thread.join()

# Close the socket
sock.close()