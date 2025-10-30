import socket
import threading
import time

# tcp_s
# CONFIGURAÇÕES DE REDE

PC_LISTEN_IP = "0.0.0.0" 

PC_LOG_PORT = 10422      # Recebe logs periódicos (JSON) da task_monitor
PC_REPORT_PORT = 10421   # Recebe relatórios de 60s da task_report_60s

ESP32_IP = "192.168.7.7" 
ESP32_COMMAND_PORT = 10421 # Porta que a ESP32 escuta para comandos do PC

# 
# FUNÇÕES DE RECEBIMENTO (SERVIDORES UDP)
# 
def log_receiver_thread():
    try:
        log_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        log_socket.bind((PC_LISTEN_IP, PC_LOG_PORT))
        print(f"Aguardando logs periódicos em {PC_LISTEN_IP}:{PC_LOG_PORT}...")
    except socket.error as err:
        print(f"Erro ao associar socket de Log (10422): {err}")
        return

    while True:
        try:
            data, addr = log_socket.recvfrom(1024)
            message = data.decode('utf-8')
            print(f"[LOG PERIÓDICO ESP32 - {addr[0]}] {message.strip()}")
        except Exception:
            break

    log_socket.close()

def report_receiver_thread():
    try:
        report_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        report_socket.bind((PC_LISTEN_IP, PC_REPORT_PORT)) 
        print(f"Aguardando Relatórios de 60s em {PC_LISTEN_IP}:{PC_REPORT_PORT}...")
    except socket.error as err:
        print(f"Erro ao associar socket de Relatório (10421): {err}")
        return

    while True:
        try:
            data, addr = report_socket.recvfrom(2048) 
            message = data.decode('utf-8')
            print("\n" + "="*80)
            print(f"== [RELATÓRIO DE 60s RECEBIDO - {addr[0]}] ==")
            print(message.strip())
            print("="*80 + "\n")
        except Exception:
            break

    report_socket.close()

# 
# FUNÇÃO DE ENVIO DE COMANDOS (CLIENTE UDP)
# 
def send_command(command: str):
    try:
        cmd_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cmd_socket.sendto(command.encode('utf-8'), (ESP32_IP, ESP32_COMMAND_PORT))
        print(f"\n[COMANDO ENVIADO] '{command}' para {ESP32_IP}:{ESP32_COMMAND_PORT}")
    except socket.error as err:
        print(f"Erro ao enviar comando: {err}")
    finally:
        cmd_socket.close()

# 
# FUNÇÃO PING/PONG PARA RTT
# 
def ping_esp():
    try:
        ping_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        ping_sock.settimeout(1.0)

        t_send = time.time()
        ping_sock.sendto(b"PING", (ESP32_IP, ESP32_COMMAND_PORT))

        data, addr = ping_sock.recvfrom(1024)
        t_recv = time.time()

        message = data.decode("utf-8").strip()
        rtt_ms = (t_recv - t_send) * 1000
        print(f"[PING] Recebido '{message}' de {addr[0]} | RTT ≈ {rtt_ms:.2f} ms")
    except socket.timeout:
        print("[PING] Sem resposta (timeout)")
    finally:
        ping_sock.close()

# 
# PROGRAMA PRINCIPAL
# 
if __name__ == "__main__":
    print(f"--- Servidor de Log UDP iniciado. ---")

    log_thread = threading.Thread(target=log_receiver_thread, daemon=True)
    log_thread.start()

    report_thread = threading.Thread(target=report_receiver_thread, daemon=True)
    report_thread.start()
    
    time.sleep(1)

    print("\n--- Interface de Controle ---")
    print(f"ESP32 IP de Destino: {ESP32_IP}")
    print("Comandos: '1: SORT_ACT', '2: SAFETY_ON', '3: ping', 'sair'")

    try:
        while True:
            cmd = input("Digite o comando > ").strip().lower()

            if cmd == "sair":
                print("Encerrando o programa...")
                break
            elif cmd == "1":
                send_command("SORT_ACT")
            elif cmd == "2":
                send_command("SAFETY_ON")
            elif cmd == "3":
                ping_esp()
            else:
                print("Comando inválido. Use '1', '2', 'ping' ou 'sair'.")
    except KeyboardInterrupt:
        print("\nPrograma encerrado por interrupção do usuário.")

    print("--- Fim da Interface de Controle. ---")
