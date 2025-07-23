# CXL Message Sender Test Module

이 모듈은 `cxl_shm.c`의 public API를 사용하여 특정 노드의 RX 버퍼로 메시지를 송신하는 테스트용 커널 모듈입니다.

## 기능

### 주요 특징
- **타겟 노드 지정**: 특정 노드로 메시지 전송
- **브로드캐스트 모드**: 모든 노드로 메시지 동시 전송
- **다양한 메시지 타입**: PING, DATA, STATUS, ECHO, BROADCAST
- **실시간 수신**: 들어오는 메시지 자동 처리
- **통계 정보**: 송신/수신 메시지 카운트

### 메시지 타입
```c
#define MSG_TYPE_PING       1    // 핑 메시지
#define MSG_TYPE_DATA       2    // 데이터 메시지
#define MSG_TYPE_STATUS     3    // 상태 메시지
#define MSG_TYPE_ECHO       4    // 에코 메시지
#define MSG_TYPE_BROADCAST  5    // 브로드캐스트 메시지
```

## 사용법

### 1. 모듈 빌드
```bash
cd /home/comsys/CXLSHMSWcoherence/msg_layer_module
make
```

### 2. CXL SHM 모듈 로드 (선행 요구사항)
```bash
# 먼저 기본 CXL SHM 모듈을 로드해야 합니다
sudo insmod cxl_shm.ko node_id=0 dax_name=dax0.0
```

### 3. 테스트 모듈 로드

#### 기본 사용 (노드 1로 메시지 전송)
```bash
sudo insmod test_cxl_sender.ko
```

#### 매개변수 지정
```bash
# 노드 2로 10개 메시지를 3초 간격으로 전송
sudo insmod test_cxl_sender.ko target_node=2 message_count=10 send_interval=3

# 브로드캐스트 모드로 5개 메시지를 2초 간격으로 전송
sudo insmod test_cxl_sender.ko enable_broadcast=true message_count=5 send_interval=2
```

## 모듈 매개변수

| 매개변수 | 타입 | 기본값 | 범위 | 설명 |
|---------|------|--------|------|------|
| `target_node` | int | 1 | 0-3 | 메시지를 전송할 타겟 노드 ID |
| `send_interval` | int | 5 | 1-60 | 메시지 전송 간격 (초) |
| `message_count` | int | 10 | 1-100 | 전송할 메시지 개수 |
| `enable_broadcast` | bool | false | true/false | 브로드캐스트 모드 활성화 |

## 사용 예시

### 예시 1: 단일 노드 테스트
```bash
# 노드 1에서 CXL SHM 모듈 로드
sudo insmod cxl_shm.ko node_id=1 dax_name=dax0.0

# 노드 0에서 테스트 모듈 로드 (노드 1로 메시지 전송)
sudo insmod test_cxl_sender.ko target_node=1 message_count=5 send_interval=2
```

### 예시 2: 브로드캐스트 테스트
```bash
# 모든 노드로 브로드캐스트
sudo insmod test_cxl_sender.ko enable_broadcast=true message_count=3 send_interval=5
```

### 예시 3: 고속 테스트
```bash
# 빠른 간격으로 많은 메시지 전송
sudo insmod test_cxl_sender.ko target_node=2 message_count=20 send_interval=1
```

## 로그 확인

### 실시간 로그 모니터링
```bash
# 모든 커널 메시지 실시간 확인
sudo dmesg -w | grep "CXL_SENDER"

# 또는 특정 로그 레벨만 확인
sudo dmesg -w | grep -E "(CXL_SENDER|shm_cxl)"
```

### 예상 로그 출력
```
CXL_SENDER: Loading CXL message sender test module
CXL_SENDER: Parameters - target_node=1, send_interval=5s, message_count=10, broadcast=disabled
CXL_SENDER: Sender thread started (target=1, interval=5s, count=10)
CXL_SENDER: Receiver thread started
CXL_SENDER: Sent PING to node 1: 'PING-0'
CXL_SENDER: Sent DATA to node 1: 'DATA-PACKET-0'
CXL_SENDER: Received ECHO from node 1: 'RESPONSE-0'
...
CXL_SENDER: Sender thread completed (10 messages sent)
CXL_SENDER: Final statistics - Sent: 20, Received: 5
```

## API 사용법

이 테스트 모듈이 사용하는 CXL SHM public API들:

### 1. 메시지 할당/해제
```c
struct cxl_kmsg_message *msg = cxl_kmsg_get(32);
// ... 메시지 설정 및 사용 ...
cxl_kmsg_put(msg);
```

### 2. 포인트-투-포인트 전송
```c
int ret = cxl_kmsg_send_message(target_node, msg, msg_size);
if (ret == 0) {
    printk("메시지 전송 성공\n");
} else {
    printk("메시지 전송 실패: %d\n", ret);
}
```

### 3. 브로드캐스트 전송
```c
int ret = cxl_kmsg_broadcast_message(msg, msg_size);
```

### 4. 메시지 수신
```c
struct cxl_kmsg_message *received_msg;
int from_node;
int ret = cxl_kmsg_poll_all_rx(&received_msg, &from_node);
if (ret == 0) {
    // 메시지 처리
    kfree(received_msg);
}
```

## 문제 해결

### 1. 모듈 로드 실패
```bash
# 의존성 확인
lsmod | grep cxl_shm

# CXL SHM 모듈이 먼저 로드되어 있는지 확인
sudo dmesg | grep "shm_cxl"
```

### 2. 메시지 전송 실패
- 타겟 노드가 올바른 node_id로 CXL SHM 모듈을 로드했는지 확인
- DAX 디바이스가 올바르게 설정되어 있는지 확인
- 네트워크 및 하드웨어 연결 상태 확인

### 3. 메시지 수신 없음
- 상대방 노드에서 메시지를 전송하고 있는지 확인
- 캐시 동기화 문제일 수 있음 - 하드웨어 CXL 설정 확인

## 모듈 언로드

```bash
# 테스트 모듈 언로드
sudo rmmod test_cxl_sender

# CXL SHM 모듈 언로드
sudo rmmod cxl_shm
```

## 주의사항

1. **순서**: 반드시 `cxl_shm.ko`를 먼저 로드한 후 `test_cxl_sender.ko`를 로드해야 합니다.
2. **노드 ID**: 각 노드는 고유한 `node_id`로 CXL SHM 모듈을 로드해야 합니다.
3. **DAX 디바이스**: 모든 노드가 동일한 DAX 디바이스를 공유해야 합니다.
4. **메모리 누수**: 수신된 메시지는 처리 후 반드시 `kfree()`로 해제됩니다.

## 확장 가능성

이 테스트 모듈을 기반으로 다음과 같은 추가 기능을 구현할 수 있습니다:

- **성능 벤치마크**: 처리량 및 지연시간 측정
- **스트레스 테스트**: 대량 메시지 전송 테스트
- **오류 복구 테스트**: 네트워크 장애 시나리오 테스트
- **사용자 정의 프로토콜**: 애플리케이션별 메시지 프로토콜 구현
