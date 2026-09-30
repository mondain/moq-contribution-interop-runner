#include "detail.h"

namespace moq::interop::http::detail {

std::string_view report_styles() {
    return "body{background:#fff;color:#17212b;font-family:sans-serif;margin:2rem;"
           "line-height:1.5}a{color:#0645ad}a:focus,summary:focus,input:focus,"
           "select:focus{outline:3px solid #111827;outline-offset:2px}"
           "table{border-collapse:collapse;width:100%}th,td{border:1px solid #6b7280;"
           "padding:.5rem;text-align:left;vertical-align:top}"
           "th{background:#dbeafe}tr:nth-child(even){background:#f3f4f6}"
           ".status{font-weight:bold}.PASS{color:#116329}.FAIL,.ERROR{color:#a40000}"
           ".PENDING,.INCOMPLETE{color:#704b00}"
           "form{background:#f3f4f6;padding:1rem;margin:1rem 0}"
           "label{display:inline-block;margin:.4rem}input,select,button{font:inherit}"
           "details{max-width:45rem}summary{cursor:pointer}"
           "code{overflow-wrap:anywhere}";
}

}  // namespace moq::interop::http::detail
