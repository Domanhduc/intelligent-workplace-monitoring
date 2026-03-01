# ============================================
# HỆ THỐNG GIÁM SÁT GHẾ LÀM VIỆC THÔNG MINH
# Phát hiện người ngồi/đứng và quản lý trạng thái ghế
# ============================================

import cv2  # Thư viện xử lý ảnh và video
import time  # Thư viện xử lý thời gian
from ultralytics import YOLO  # Thư viện YOLO để nhận diện đối tượng
import paho.mqtt.client as mqtt  # Thư viện MQTT để giao tiếp với ESP32

# Khởi tạo model YOLO (yolov8n = nano version, nhẹ và nhanh)
model = YOLO("yolov8n.pt")

# ============================================
# CẤU HÌNH MQTT KẾT NỐI TỚI ESP32
# ============================================
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 1883
MQTT_TOPIC_SEAT = "domanhduc/room1/seat/status"  # Trùng với topic_seat trên ESP32, tránh trùng với người khác trên broker public


def on_connect(client, userdata, flags, rc):
    """Log khi MQTT kết nối thành công (kể cả sau reconnect)."""
    print("[MQTT] Connected with rc =", rc)


def on_disconnect(client, userdata, rc):
    """Đánh dấu mất kết nối, việc reconnect sẽ xử lý ở vòng lặp chính."""
    global mqtt_disconnected
    print("[MQTT] Disconnected with rc =", rc)
    mqtt_disconnected = True


mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()  # Chạy loop MQTT ở thread riêng

def calculate_iou(box1, box2):
    """
    Tính Intersection over Union (IoU) giữa 2 bounding boxes
    IoU = Diện tích giao nhau / Diện tích hợp nhất
    Trả về giá trị từ 0.0 đến 1.0
    """
    x1_1, y1_1, x2_1, y2_1 = box1  # Tọa độ box 1: (x1, y1) góc trên trái, (x2, y2) góc dưới phải
    x1_2, y1_2, x2_2, y2_2 = box2  # Tọa độ box 2
    
    # Tính tọa độ vùng giao nhau của 2 box
    x1_inter = max(x1_1, x1_2)  # Góc trên trái X của vùng giao
    y1_inter = max(y1_1, y1_2)  # Góc trên trái Y của vùng giao
    x2_inter = min(x2_1, x2_2)  # Góc dưới phải X của vùng giao
    y2_inter = min(y2_1, y2_2)  # Góc dưới phải Y của vùng giao
    
    # Kiểm tra xem 2 box có giao nhau không
    if x2_inter <= x1_inter or y2_inter <= y1_inter:
        return 0.0  # Không giao nhau
    
    # Tính diện tích vùng giao nhau
    area_inter = (x2_inter - x1_inter) * (y2_inter - y1_inter)
    # Tính diện tích từng box
    area1 = (x2_1 - x1_1) * (y2_1 - y1_1)
    area2 = (x2_2 - x1_2) * (y2_2 - y1_2)
    # Diện tích hợp nhất = tổng 2 box - vùng giao 
    area_union = area1 + area2 - area_inter
    
    # Trả về tỷ lệ IoU
    return area_inter / area_union if area_union > 0 else 0.0

def calculate_area(box):
    """
    Tính diện tích của một bounding box
    """
    x1, y1, x2, y2 = box
    return (x2 - x1) * (y2 - y1)

def check_person_on_chair(person_box, chair_box):
    """
    Kiểm tra xem người có đang ngồi trên ghế không
    Sử dụng nhiều điều kiện để đảm bảo độ chính xác
    """
    # Điều kiện 1: Kiểm tra IoU (nếu > 0.25 thì coi như đang ngồi)
    if calculate_iou(person_box, chair_box) > 0.25:
        return True
    
    # Điều kiện 2: Kiểm tra overlap và vị trí chi tiết hơn
    p_x1, p_y1, p_x2, p_y2 = person_box  # Tọa độ người
    c_x1, c_y1, c_x2, c_y2 = chair_box   # Tọa độ ghế
    
    # Kiểm tra xem có overlap theo trục X không (người và ghế có chồng lên nhau theo chiều ngang)
    x_overlap = not (p_x2 < c_x1 or p_x1 > c_x2)
    if x_overlap:
        # Tính độ rộng vùng chồng lên nhau
        overlap_width = min(p_x2, c_x2) - max(p_x1, c_x1)
        chair_width = c_x2 - c_x1
        # Nếu overlap >= 50% chiều rộng ghế
        if overlap_width / chair_width > 0.5:
            # Tính trung tâm theo trục Y
            p_center_y = (p_y1 + p_y2) / 2  # Trung tâm người
            c_center_y = (c_y1 + c_y2) / 2  # Trung tâm ghế
            # Kiểm tra người có ở phía trên ghế không (trung tâm người cao hơn trung tâm ghế)
            if p_center_y < c_center_y:
                distance_y = c_center_y - p_center_y  # Khoảng cách theo trục Y
                chair_height = c_y2 - c_y1
                # Khoảng cách không quá xa (trong phạm vi 1.5 lần chiều cao ghế)
                if distance_y < chair_height * 1.5:
                    return True
    return False

# ============================================
# KHỞI TẠO CAMERA VÀ CỬA SỔ HIỂN THỊ
# ============================================

# Kết nối với camera ESP32-CAM qua stream HTTP
cap = cv2.VideoCapture("http://192.168.2.103:81/stream", cv2.CAP_FFMPEG)
cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # Giảm buffer để giảm độ trễ

# Tạo cửa sổ hiển thị video
cv2.namedWindow("ESP32-CAM", cv2.WINDOW_NORMAL)
cv2.resizeWindow("ESP32-CAM", 800, 600)  # Kích thước cửa sổ

# ============================================
# KHAI BÁO BIẾN TOÀN CỤC
# ============================================

fixed_chair_box = None  # Vị trí ghế cố định (sau khi đã khóa)
chair_locked = False  # Cờ xác định ghế đã được khóa chưa

# State machine: WORKING → COUNTDOWN → LEFT_SEAT
# WORKING: Người đang ngồi trên ghế
# COUNTDOWN: Ghế trống, đang đếm ngược 60 giây
# LEFT_SEAT: Ghế trống hơn 60 giây
seat_status = "UNKNOWN"
countdown_start_time = None  # Thời điểm bắt đầu đếm ngược
COUNTDOWN_DURATION = 60  # Thời gian đếm ngược (60 giây)
CHAIR_DETECT_TIMEOUT = 10  # Timeout phát hiện ghế (10 giây)
chair_detect_start_time = time.time()  # Thời điểm bắt đầu tìm ghế

# Danh sách các đối tượng được phát hiện
person_boxes = []  # Danh sách bounding box của người
person_on_chair = False  # Cờ xác định có người đang ngồi không
sitting_flags = []  # Lưu kết quả check_person_on_chair để tránh tính lại (tối ưu CPU)

# Debounce trạng thái: Cần xác nhận liên tiếp N frame để tránh nhảy trạng thái do frame lỗi
SIT_CONFIRM_FRAMES = 3  # Số frame cần xác nhận để chuyển sang trạng thái "ngồi"
LEAVE_CONFIRM_FRAMES = 3  # Số frame cần xác nhận để chuyển sang trạng thái "rời"
sit_confirm_count = 0  # Đếm số frame liên tiếp phát hiện người ngồi
leave_confirm_count = 0  # Đếm số frame liên tiếp phát hiện người rời

# Biến lưu trạng thái đã publish lên MQTT (tránh spam)
last_published_seat_status = None

# Cờ báo MQTT đang bị ngắt, reconnect sẽ xử lý trong vòng lặp chính
mqtt_disconnected = False

# Điều khiển tần suất chạy YOLO theo thời gian (ổn định hơn so với theo frame)
last_yolo_time = 0.0
YOLO_INTERVAL = 0.2  # giây, ~5 Hz nếu đủ FPS

# Keepalive cho MQTT: định kỳ gửi lại trạng thái ghế hiện tại
last_keepalive_time = 0.0
KEEPALIVE_INTERVAL = 3.0  # giây

# Biến để reconnect camera nếu mất frame liên tục
camera_fail_count = 0
CAMERA_FAIL_THRESHOLD = 30  # số lần fail liên tiếp trước khi reopen stream

# ============================================
# VÒNG LẶP CHÍNH - XỬ LÝ VIDEO
# ============================================

while True:
    # Thử reconnect MQTT nếu trước đó bị ngắt
    if mqtt_disconnected:
        try:
            mqtt_client.reconnect()
            mqtt_disconnected = False
            print("[MQTT] Reconnected!")
        except Exception:
            # Nếu vẫn chưa reconnect được thì bỏ qua, thử lại ở vòng lặp sau
            pass

    # Đọc frame từ camera
    ret, frame = cap.read()
    if not ret:
        print("⚠️ Lost camera frame, retrying...")
        camera_fail_count += 1
        if camera_fail_count > CAMERA_FAIL_THRESHOLD:
            print("⚠️ Camera stream seems dead. Reopening...")
            cap.release()
            cap = cv2.VideoCapture("http://192.168.2.103:81/stream", cv2.CAP_FFMPEG)
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            camera_fail_count = 0
            time.sleep(1.0)
        else:
            time.sleep(0.2)
        continue

    # Đọc frame thành công thì reset bộ đếm lỗi camera
    camera_fail_count = 0

    # Resize frame để tăng tốc độ xử lý (320x240 thay vì kích thước gốc)
    frame = cv2.resize(frame, (320, 240))

    # Chỉ chạy YOLO sau mỗi khoảng thời gian cố định để tối ưu hiệu năng (giảm tải CPU)
    current_time = time.time()
    if current_time - last_yolo_time >= YOLO_INTERVAL:
        last_yolo_time = current_time
        # Điều chỉnh confidence threshold:
        # - Khi chưa khóa ghế: conf=0.5 (cao hơn) để tránh nhận nhầm vật giống ghế
        # - Khi đã khóa ghế: conf=0.4 (thấp hơn) để detect người tốt hơn
        conf = 0.5 if not chair_locked else 0.4
        
        # Chạy YOLO để phát hiện đối tượng
        # - Class 0: Person (người)
        # - Class 56: Chair (ghế) - chỉ detect khi chưa khóa ghế
        results = model.predict(
            frame,
            classes=[0, 56] if not chair_locked else [0],  # Chỉ detect person nếu đã khóa ghế
            conf=conf,
            imgsz=320,
            device="cpu",  # Chạy trên CPU
            verbose=False  # Không hiển thị log
        )

        # Làm sạch danh sách từ frame trước
        person_boxes = []
        chair_boxes = []

        # Duyệt qua tất cả kết quả phát hiện
        for r in results:
            for box in r.boxes:
                class_id = int(box.cls[0])  # ID của class (0=person, 56=chair)
                box_coords = box.xyxy[0].cpu().numpy().astype(int)  # Tọa độ bounding box (int để tính hình học ổn định hơn)
                
                if class_id == 0:  # Phát hiện người
                    person_boxes.append(box_coords)
                elif class_id == 56 and not chair_locked:  # Phát hiện ghế (chỉ khi chưa khóa)
                    chair_boxes.append(box_coords)
        
        # ============================================
        # PHÁT HIỆN VÀ KHÓA GHẾ
        # ============================================
        if not chair_locked:
            if chair_boxes:
                # Chọn ghế lớn nhất (có diện tích lớn nhất) làm ghế làm việc chính
                fixed_chair_box = max(chair_boxes, key=calculate_area).copy()
                chair_locked = fixed_chair_box is not None
                print(f"Đã khóa ghế: {fixed_chair_box}")
            else:
                # Kiểm tra timeout: Nếu sau 10 giây vẫn không phát hiện được ghế
                if time.time() - chair_detect_start_time >= CHAIR_DETECT_TIMEOUT:
                    print("⚠️ Không phát hiện được ghế sau 10 giây. Vui lòng điều chỉnh camera.")
                    chair_detect_start_time = time.time()
        
        # ============================================
        # TỐI ƯU: KIỂM TRA NGƯỜI NGỒI (chỉ tính 1 lần)
        # ============================================
        # Tính check_person_on_chair một lần và lưu kết quả vào sitting_flags
        # Giúp giảm ~30-40% phép tính hình học (không cần tính lại khi vẽ)
        sitting_flags = []
        if fixed_chair_box is not None and person_boxes:
            for person_box in person_boxes:
                is_sitting = check_person_on_chair(person_box, fixed_chair_box)
                sitting_flags.append(is_sitting)
            person_on_chair = any(sitting_flags)  # Có ít nhất 1 người đang ngồi
        else:
            sitting_flags = [False] * len(person_boxes) if person_boxes else []
            person_on_chair = False
        
        # ============================================
        # STATE MACHINE VỚI DEBOUNCE
        # Quản lý trạng thái: WORKING → COUNTDOWN → LEFT_SEAT
        # ============================================
        if not chair_locked:
            # Chưa khóa ghế: trạng thái ghế chưa xác định
            seat_status = "UNKNOWN"
            countdown_start_time = None
            sit_confirm_count = 0
            leave_confirm_count = 0
        elif person_on_chair:
            # Có người đang ngồi trên ghế
            # Debounce: Cần xác nhận liên tiếp N frame để tránh nhảy trạng thái
            sit_confirm_count += 1
            leave_confirm_count = 0  # Reset counter rời ghế
            
            if sit_confirm_count >= SIT_CONFIRM_FRAMES:
                # Đã xác nhận đủ số frame → Chuyển sang WORKING
                if seat_status == "COUNTDOWN":
                    countdown_start_time = None  # Hủy đếm ngược
                seat_status = "WORKING"
        else:
            # Không có người trên ghế
            # Debounce: Cần xác nhận liên tiếp N frame
            leave_confirm_count += 1
            sit_confirm_count = 0  # Reset counter ngồi ghế
            
            if leave_confirm_count >= LEAVE_CONFIRM_FRAMES:
                # Đã xác nhận đủ số frame
                if seat_status == "WORKING":
                    # Chuyển từ WORKING sang COUNTDOWN (bắt đầu đếm ngược)
                    seat_status = "COUNTDOWN"
                    countdown_start_time = current_time
                elif seat_status == "COUNTDOWN" and countdown_start_time:
                    # Kiểm tra xem đã hết thời gian đếm ngược chưa
                    if current_time - countdown_start_time >= COUNTDOWN_DURATION:
                        # Đã hết 60 giây → Chuyển sang LEFT_SEAT
                        seat_status = "LEFT_SEAT"
                        countdown_start_time = None

        # ============================================
        # GỬI TRẠNG THÁI GHẾ LÊN MQTT CHO ESP32
        # Chỉ publish khi trạng thái thay đổi để không spam broker
        # ============================================
        if seat_status != last_published_seat_status:
            try:
                mqtt_client.publish(MQTT_TOPIC_SEAT, seat_status, qos=0, retain=False)
                print(f"[MQTT] Published seat status: {seat_status}")
                last_published_seat_status = seat_status
            except Exception as e:
                print(f"[MQTT] Error publish seat status: {e}")

    # ============================================
    # MQTT KEEPALIVE: gửi lại trạng thái ghế định kỳ để ESP32 không timeout
    # ============================================
    now_keepalive = time.time()
    if now_keepalive - last_keepalive_time >= KEEPALIVE_INTERVAL:
        last_keepalive_time = now_keepalive
        try:
            mqtt_client.publish(MQTT_TOPIC_SEAT, seat_status, qos=0, retain=False)
            print(f"[MQTT] Keepalive seat status: {seat_status}")
        except Exception as e:
            print(f"[MQTT] Keepalive publish error: {e}")

    # ============================================
    # VẼ BOUNDING BOX VÀ HIỂN THỊ THÔNG TIN
    # ============================================
    
    # Vẽ bounding box cho ghế cố định (nếu đã khóa)
    if fixed_chair_box is not None:
        x1, y1, x2, y2 = map(int, fixed_chair_box)
        # Màu sắc theo trạng thái:
        # - Vàng (0, 255, 255): WORKING (có người ngồi)
        # - Cam (0, 165, 255): COUNTDOWN (đang đếm ngược)
        # - Đỏ (0, 0, 255): LEFT_SEAT (ghế trống > 60s)
        color = (0, 255, 255) if seat_status == "WORKING" else (0, 165, 255) if seat_status == "COUNTDOWN" else (0, 0, 255)
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 3)  # Vẽ hình chữ nhật
        cv2.putText(frame, "Fixed Chair", (x1, y1-5), cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)  # Vẽ nhãn
    
    # Vẽ bounding box cho người
    # Sử dụng sitting_flags đã tính sẵn (tối ưu CPU - giảm ~30-40% phép tính hình học)
    for i, person_box in enumerate(person_boxes):
        x1, y1, x2, y2 = map(int, person_box)
        # Lấy kết quả từ sitting_flags (đã tính sẵn, không cần tính lại)
        is_sitting = sitting_flags[i] if i < len(sitting_flags) else False
        # Màu sắc:
        # - Vàng (0, 255, 255): Đang ngồi
        # - Xanh lá (0, 255, 0): Đang đứng
        color = (0, 255, 255) if is_sitting else (0, 255, 0)
        label = "Person (Sitting)" if is_sitting else "Person"
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
        cv2.putText(frame, label, (x1, y1-5), cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)

    # ============================================
    # HIỂN THỊ TRẠNG THÁI TRÊN MÀN HÌNH
    # ============================================
    
    # Màu sắc và text cho trạng thái ghế
    status_color = (0, 255, 0) if seat_status == "WORKING" else (0, 165, 255) if seat_status == "COUNTDOWN" else (0, 0, 255)
    status_text = f"Status: {seat_status}"
    # Nếu đang đếm ngược, hiển thị thời gian còn lại
    if seat_status == "COUNTDOWN" and countdown_start_time:
        remaining = max(0, COUNTDOWN_DURATION - (time.time() - countdown_start_time))
        status_text = f"Status: {seat_status} ({remaining:.1f}s)"
    
    cv2.putText(frame, status_text, (5, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.5, status_color, 1)
    
    # Hiển thị trạng thái người
    if chair_locked:
        person_status = "SITTING" if person_on_chair else "NOT SITTING"
        person_color = (0, 255, 255) if person_on_chair else (0, 0, 255)
        cv2.putText(frame, f"Person: {person_status}", (5, 35), cv2.FONT_HERSHEY_SIMPLEX, 0.5, person_color, 1)
    else:
        # Hiển thị trạng thái phát hiện ghế
        if chair_detect_start_time and time.time() - chair_detect_start_time >= CHAIR_DETECT_TIMEOUT:
            cv2.putText(frame, "Please adjust camera", (5, 35), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0,0,255), 1)
        else:
            cv2.putText(frame, "Detecting chair...", (5, 35), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0,165,255), 1)

    # Hiển thị frame lên cửa sổ
    cv2.imshow("ESP32-CAM", frame)

    # ============================================
    # XỬ LÝ PHÍM BẤM
    # ============================================
    key = cv2.waitKey(1) & 0xFF
    if key == ord('q'):
        # Nhấn 'q' để thoát
        break
    elif key == ord('r'):
        # Nhấn 'r' để reset và tìm lại ghế
        fixed_chair_box = None
        chair_locked = False
        seat_status = "UNKNOWN"
        countdown_start_time = None
        chair_detect_start_time = time.time()
        sit_confirm_count = 0
        leave_confirm_count = 0
        sitting_flags = []
        print("Đã reset ghế")

# Giải phóng tài nguyên
cap.release()  # Đóng kết nối camera
cv2.destroyAllWindows()  # Đóng tất cả cửa sổ
