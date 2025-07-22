# CXL Shared Memory Messaging Layer

## 개요
CXL 공유 메모리를 통한 노드 간 메시지 통신 커널 모듈입니다.

## 빌드 및 사용법

### 빌드
```bash
make clean && make
```

### 모듈 로드
```bash
# 기본 설정
sudo insmod cxl_shm.ko

# 매개변수 지정
sudo insmod cxl_shm.ko node_id=42 dax_name=dax0.0

# 로그 확인
dmesg | tail -5
```

### 모듈 언로드
```bash
sudo rmmod cxl_shm
```

## 매개변수
- `node_id`: CXL 노드 ID (기본값: 0)
- `dax_name`: DAX 디바이스 이름 (기본값: "dax0.0")

## API 함수
- `cxl_kmsg_get(size)`: 메시지 할당
- `cxl_kmsg_put(msg)`: 메시지 해제  
- `cxl_kmsg_send_message(dest_nid, msg, size)`: 메시지 전송

## 캐시 관리
- **쓰기 후**: `flush_processor_cache()` - 다른 노드가 볼 수 있도록
- **읽기 전**: `invalidate_processor_cache()` - 최신 데이터 확인
- **초기화**: `hard_flush_processor_cache()` - 양방향 플러시
