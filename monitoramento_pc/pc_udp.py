import socket
import threading
import time

# 
#  CONFIGURAÇÕES DE REDE
# 

PC_LISTEN_IP = "0.0.0.0"          # Escuta em todas as interfaces de rede do PC

PC_LOG_PORT = 10422               # Porta que recebe logs periódicos da task_monitor (ESP32 -> PC)
PC_REPORT_PORT = 10421            # Porta que recebe relatórios de 60s da task_report_60s (ESP32 -> PC)

ESP32_IP = "10.81.100.217"        # IP da ESP32 (ajustar conforme sua rede)
ESP32_COMMAND_PORT = 10421        # Porta da ESP32 que escuta comandos vindos do PC


# 
#  THREAD DE RECEBIMENTO DE LOGS PERIÓDICOS (task_monitor)
# 
def log_receiver_thread():
    try:
        # Cria socket UDP para receber logs
        log_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        log_socket.bind((PC_LISTEN_IP, PC_LOG_PORT))  # Liga socket à porta 10422
        print(f"Aguardando logs periódicos em {PC_LISTEN_IP}:{PC_LOG_PORT}...")
    except socket.error as err:
        print(f"Erro ao associar socket de Log (10422): {err}")
        return

    # Loop infinito de recepção
    while True:
        try:
            # Espera receber até 1024 bytes (não bloqueia o programa principal)
            data, addr = log_socket.recvfrom(1024)
            message = data.decode('utf-8')             # Decodifica bytes em string UTF-8
            print(f"[LOG PERIÓDICO ESP32 - {addr[0]}] {message.strip()}")
        except Exception:
            break  # Sai do loop se o socket for fechado ou erro ocorrer

    log_socket.close()  # Fecha o socket ao encerrar


# 
#  THREAD DE RECEBIMENTO DE RELATÓRIOS DE 60s (task_report_60s)
# 
def report_receiver_thread():
    try:
        # Cria socket UDP para relatórios
        report_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        report_socket.bind((PC_LISTEN_IP, PC_REPORT_PORT))  # Porta 10421
        print(f"Aguardando Relatórios de 60s em {PC_LISTEN_IP}:{PC_REPORT_PORT}...")
    except socket.error as err:
        print(f"Erro ao associar socket de Relatório (10421): {err}")
        return

    # Loop infinito de recepção
    while True:
        try:
            # Recebe mensagens de até 2048 bytes
            data, addr = report_socket.recvfrom(2048)
            message = data.decode('utf-8')

            # Exibe o relatório formatado no terminal
            print("\n" + "="*80)
            print(f"== [RELATÓRIO DE 60s RECEBIDO - {addr[0]}] ==")
            print(message.strip())
            print("="*80 + "\n")
        except Exception:
            break

    report_socket.close()


# 
#  FUNÇÃO DE ENVIO DE COMANDOS (CLIENTE UDP)
# 
def send_command(command: str):
    try:
        # Cria socket UDP temporário
        cmd_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Envia comando como string codificada em UTF-8
        cmd_socket.sendto(command.encode('utf-8'), (ESP32_IP, ESP32_COMMAND_PORT))

        print(f"\n[COMANDO ENVIADO] '{command}' para {ESP32_IP}:{ESP32_COMMAND_PORT}")
    except socket.error as err:
        print(f"Erro ao enviar comando: {err}")
    finally:
        cmd_socket.close()  # Fecha o socket após envio


# 
#  FUNÇÃO DE PING/PONG PARA TESTAR RTT (Round Trip Time)
# 
def ping_esp():
    try:
        ping_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        ping_sock.settimeout(1.0)  # Timeout de 1 segundo

        # Envia pacote de "PING" e marca o tempo de envio
        t_send = time.time()
        ping_sock.sendto(b"PING", (ESP32_IP, ESP32_COMMAND_PORT))

        # Aguarda resposta (espera "PONG" da ESP32)
        data, addr = ping_sock.recvfrom(1024)
        t_recv = time.time()  # Marca tempo de resposta

        # Calcula o RTT (tempo total ida e volta)
        message = data.decode("utf-8").strip()
        rtt_ms = (t_recv - t_send) * 1000
        print(f"[PING] Recebido '{message}' de {addr[0]} | RTT ≈ {rtt_ms:.2f} ms")
    except socket.timeout:
        print("[PING] Sem resposta (timeout)")  # ESP32 não respondeu a tempo
    finally:
        ping_sock.close()


# 
#  PROGRAMA PRINCIPAL
# 
if __name__ == "__main__":
    print(f"--- Servidor de Log UDP iniciado. ---")

    # Cria e inicia threads de recepção
    log_thread = threading.Thread(target=log_receiver_thread, daemon=True)
    log_thread.start()

    report_thread = threading.Thread(target=report_receiver_thread, daemon=True)
    report_thread.start()
    
    # Espera um pouco para estabilizar os sockets
    time.sleep(1)

    print("\n--- Interface de Controle ---")
    print(f"ESP32 IP de Destino: {ESP32_IP}")
    print("Comandos: '1: SORT_ACT', '2: SAFETY_ON', '3: ping', 'sair'")

    try:
        # Loop principal do menu
        while True:
            cmd = input("Digite o comando > ").strip().lower()

            if cmd == "sair":
                print("Encerrando o programa...")
                break
            elif cmd == "1":
                send_command("SORT_ACT")   # Ativa atuador de seleção
            elif cmd == "2":
                send_command("SAFETY_ON")  # Liga modo de segurança
            elif cmd == "3":
                ping_esp()                 # Faz teste de ping
            else:
                print("Comando inválido. Use '1', '2', 'ping' ou 'sair'.")
    except KeyboardInterrupt:
        # Encerra com Ctrl + C
        print("\nPrograma encerrado por interrupção do usuário.")

    print("--- Fim da Interface de Controle. ---")
