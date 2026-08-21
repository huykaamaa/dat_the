#pragma once

void handleRoot();

// Escape truoc khi nhet gia tri do NGUOI KHAC kiem soat vao HTML - ten SSID cua AP la nga
// nhau: mot AP hang xom dat ten chua the <script> ma dashboard in tho ra thi trang tu chay ma
// do. Dung chung boi html.cpp va web.cpp.
String htmlEscape(const String &s);
