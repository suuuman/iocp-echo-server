#pragma once
//
//  지표 노출.
//
//  로그는 사람이 읽는 것이고, 이것은 수집 도구가 읽는 것이다.
//  5초마다 한 줄씩 남기는 로그로는 추세도 경보도 만들 수 없다.
//
//  이름 · 설명 · 종류를 함께 내보내는 형식을 따른다.
//    counter - 계속 늘기만 하는 값. 증가율을 본다
//    gauge   - 오르내리는 값. 그 시점의 상태를 본다
//
//  백분위는 계산해서 내보낸다. 원시 표본을 내보내려면 크기가 감당되지 않고,
//  받는 쪽에서 다시 분포를 만들어야 한다.
//
#include <cstdint>
#include <string>

#include "app/service_router.h"
#include "db/db_worker_pool.h"
#include "net/io_service.h"

namespace app {

// 관리 스레드에서 호출된다. 각 계층이 공개한 값만 읽는다
std::string render_metrics(net::IoService& io, const ServiceRouter& services,
                           const db::WorkerPool& db, std::int64_t uptime_ms);

} // namespace app
