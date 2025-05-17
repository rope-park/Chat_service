# 네트워크프로그래밍 텀프로젝트: C 기반 채팅 서비스

## 📋 프로젝트 개요
대학교 네트워크프로그래밍 수업 최종 기말 프로젝트로, C 언어로 구현된 서버·클라이언트 채팅 프로그램과 추후 PWA(Web) 클라이언트를 연동하는 풀스택 채팅 서비스입니다.

- **기간**: 2025.05 ~ 2025.06 
- **팀/개인**: 개인 프로젝트 (박주을, 컴퓨터공학부)  
- **환경**: Ubuntu (VMware)  
- **버전 관리**: Git  

## 🎯 주요 목표 및 요구사항
1. **MVP 단계**  
   - WebSocket 기반 실시간 채팅  
   - 메시지 영속화 (SQLite3 DB 저장·복원)
2. **확장 기능**  
   - 이미지 업로드·전송
   - TLS 암호화 (OpenSSL 기반 HTTPS/WSS)  
   - JWT 인증/인가  
   - SQL Injection, XSS 방어 및 Rate Limiting  

## 🏗️ 시스템 아키텍처

클라이언트 ←→ WebSocket 서버 ←→ Core Engine ←→ SQLite3 DB
│                                      ↑
└─── HTTP/REST API (이미지 업로드 등) ──┘

- **서버**: C, pthread, epoll  
- **DB**: SQLite3 (C/C++ 인터페이스)  
- **클라이언트 (CLI)**: C, 표준 TCP 소켓  
- **클라이언트 (GUI → 계획 중)**: React.js, TypeScript, TailwindCSS, PWA 

## 💻 기술 스택
| 영역            | 기술 및 라이브러리                               |
| --------------- | ----------------------------------------------- |
| 서버 언어       | C (pthread, epoll)                              |
| 데이터베이스    | SQLite3 (C API)                                 |
| 보안            | OpenSSL (HTTPS/WSS), JWT                        |
| 클라이언트 (CLI)| C, GTK+ (추가 GUI 버전 구현 가능)               |
| 클라이언트 (Web)| React.js · TypeScript · TailwindCSS · PWA (예정) |
| 버전 관리       | Git                                             |

## 🚀 설치 및 실행 방법

1. **리포지토리 클론**
   ```bash
   git clone https://github.com/<username>/network-chat-term.git
   cd network-chat-term

2. **서버 빌드**

   ```bash
     make
   ```

   * `Makefile`에 정의된 `chat_server` 타겟으로 빌드됩니다.

3. **서버 실행**

   ```bash
   ./chat_server
   ```

   * 기본 포트: `9000` (소스 코드에서 `PORTNUM` 매크로로 변경 가능)

4. **CLI 클라이언트 사용 (nc)**

   ```bash
   nc localhost 9000
   ```

   * 접속 후, 닉네임 입력 → `/help` 명령어로 사용 가능한 커맨드 확인

5. **추가 GUI 클라이언트**

   ```bash
   gcc chat_client_gtk.c -o chat_client_gtk `pkg-config --cflags --libs gtk+-3.0`
   ./chat_client_gtk
   ```

## 🔧 주요 디렉터리 구조

```
.
├── src/
│   ├── chat_server.c         # 서버 메인 구현
│   ├── chatting_client_cli.c # CLI 클라이언트
│   ├── chat_client_gtk.c     # GTK+ GUI 클라이언트
│   └── ...                   
├── include/                  # 헤더 파일
├── Makefile                  # 빌드 스크립트
├── README.md                 
└── docs/
    ├── 네트워크프로그래밍10.pdf
    └── 네트워크프로그래밍11.pdf
```

## 🗄️ 데이터베이스 스키마

```sql
-- 채팅 메시지 저장 테이블
CREATE TABLE chat_messages (
  message_id   INTEGER PRIMARY KEY AUTOINCREMENT,
  room_id      INTEGER NOT NULL,
  sender       VARCHAR(50),
  content      TEXT,
  ts           TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 파일 (이미지) 관리 테이블
CREATE TABLE chat_files (
  file_id      INTEGER PRIMARY KEY AUTOINCREMENT,
  message_id   INTEGER,
  file_url     TEXT,
  file_type    TEXT,
  upload_ts    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

## 📈 향후 계획

* **Web 클라이언트 (PWA)**

  * React.js + TypeScript + TailwindCSS 기반
  * 오프라인 지원, 푸시 알림, 반응형 UI
* **관리자 페이지**

  * 채팅방 관리(삭제, 제목 변경, 강퇴 등)
* **성능 최적화 및 배포**

  * Docker 컨테이너화
  * CI/CD 파이프라인 구축

## 🤝 기여 방법

1. Fork & Clone 리포지토리
2. 새로운 브랜치 생성 (`git checkout -b feature/your-feature`)
3. 코드 수정 후 커밋 (`git commit -m "Add some feature"`)
4. Push & Pull Request 생성

## 📄 라이선스

본 프로젝트는 [MIT 라이선스](LICENSE) 하에 배포됩니다.



- **참고자료**  

