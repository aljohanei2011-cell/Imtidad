# خطة تنفيذ ScreenLink

## المرحلة A — الأساس (ضمن هذه الحزمة)
- تطبيق Android يعرض اتصال TCP ونتيجة المصافحة واختيار الأوضاع الثلاثة.
- خادم Windows يدير اتصالات TCP وPING/PONG وSET_MODE.
- تعريف بروتوكول رسائل مشترك.

## المرحلة B — انعكاس الشاشة
- Capture: Windows Graphics Capture أو DXGI Desktop Duplication.
- Encoder: Media Foundation H.264 hardware acceleration.
- Transport: WebRTC مع DTLS-SRTP ودعم إعادة الاتصال.
- Android: WebRTC receiver / MediaCodec renderer.
- Quality adaptation, cursor and orientation.

## المرحلة C — التحكم عن بعد
- قناة DataChannel آمنة، تحويل الإحداثيات وفق aspect ratio وDPI.
- Win32 SendInput للماوس ولوحة المفاتيح ضمن حدود النظام.
- تأكيد صلاحية التحكم، منع الأوامر غير المصرح بها، مراقبة الأحداث.

## المرحلة D — توسيع الشاشة
- Windows Indirect Display Driver (IddCx + UMDF).
- الإعلان عن أوضاع عرض مدعومة، استلام swapchains وتمرير الإطارات لخدمة البث.
- التوقيع وتثبيت التعريف واختبارات التوافق وإزالة التعريف بأمان.

## المرحلة E — الصوت وUSB والتوزيع
- WASAPI Loopback مع مزامنة A/V.
- USB debugging/ADB فقط للتطوير؛ تصميم مسار USB إنتاجي منفصل.
- مثبت Windows، حزم Android، CI، اختبارات الأمن والأداء.

## معايير القبول المقترحة
- اتصال موثق ومشفر قبل التشغيل الإنتاجي.
- 1080p/60fps هدف اختباري، وليس وعدًا بالأداء.
- قياس زمن الاستجابة من إدخال المستخدم إلى ظهور النتيجة.
- تشغيل مستقر لمدة ساعتين على مجموعة أجهزة اختبار معروفة.
