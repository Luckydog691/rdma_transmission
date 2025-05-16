/home/cyh/mlnx_tools/mlnx_list.sh

gcc -Wall -O2 -o server server.c -libverbs
gcc -Wall -O2 -o client client.c -libverbs

gcc -Wall -O2 -o server_baseline server_baseline.c -libverbs
gcc -Wall -O2 -o client_baseline client_baseline.c -libverbs

ib_send_bw -d mlx5_3
ib_send_bw -d mlx5_4 192.168.20.123

./server -g 0 -i 1 -d mlx5_3
./client 192.168.20.123 -g 0 -d mlx5_4

./server_baseline -g 0 -i 1 -d mlx5_3
./client_baseline 192.168.20.123 -g 0 -d mlx5_4

/usr/bin/time -v ./server_baseline -g 0 -i 1 -d mlx5_3
/usr/bin/time -v ./client_baseline 192.168.20.123 -g 0 -d mlx5_4

pidstat -r  1339812 1

ps aux | grep "./client"
ps aux | grep "./server -g 0"
ps aux | grep "./server -g 0" | awk '{print $2}' | xargs kill

./server -g 0 -i 1 -d mlx5_3
ps aux | grep "./server -g 0" | awk '{print $2}' | xargs kill