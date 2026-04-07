### clean 설치
소스 루트로 이동
cd /home/comsys/famfs

기존 빌드 디렉토리 삭제 후 재생성
rm -rf debug
mkdir debug
cd debug

CMake + 빌드
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

### 재설치
cd /home/comsys/famfs
make -C debug -j"$(nproc)"