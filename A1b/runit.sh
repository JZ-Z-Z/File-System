mkdir filesystem
truncate -s 65536 disk.img
./mkfs.a1fs -i 16 disk.img
./a1fs disk.img filesystem
cd filesystem
mkdir folder1
mkdir folder2
touch file1
truncate -s 4096 file1
ls
mv file1 folder1
cd folder1
touch file2
ls
stat file1
cd ..
mv folder1 folder2
ls
cd folder2
ls
cd ..
cd ..
fusermount -u filesystem
./a1fs disk.img filesystem
cd filesystem
ls
cd ..
