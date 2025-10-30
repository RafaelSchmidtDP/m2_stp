import socket
import threading
import json
import time

# 
#  CONFIGURAÇÕES DE CONEXÃO TCP
# 
ESP32_IP = "10.81.100.217"   # Endereço IP da ESP32
TCP_PORT = 5000              # Porta usada para conexão TCP (configurada na ESP32)
RECV_TIMEOUT = 1.0           # Timeout para leitura do socket (segundos)
stop_thread = False          # Flag global para encerrar threads com segurança

# Variável global usada para medir o tempo de resposta (RTT)
last_ping_time = None


# 
#  THREAD DE RECEBIMENTO TCP
# 
def recv_tcp(sock):
    """Recebe logs JSON, relatórios e respostas PONG da ESP32 via TCP."""
    global stop_thread, last_ping_time
    buffer = ""  # Buffer para acumular dados parciais entre pacotes

    # Loop principal de recepção
    while not stop_thread:
        try:
            data = sock.recv(4096)  # Lê até 4 KB de dados
            if not data:
                # Se não chegou nada, o servidor fechou a conexão
                print("[TCP] Conexão encerrada pelo ESP32")
                stop_thread = True
                break

            # Decodifica bytes em string e adiciona ao buffer
            buffer += data.decode("utf-8", errors="ignore")

            # Processa linhas completas (separadas por '\n')
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                line = line.strip()
                if not line:
                    continue

                # 
                # 1. Resposta ao comando "PING" -> calcula RTT
                # 
                if line.startswith("PONG"):
                    if last_ping_time is not None:
                        rtt = (time.time() - last_ping_time) * 1000
                        print(f"[PING TCP] Recebido '{line}' | RTT ≈ {rtt:.2f} ms")
                        last_ping_time = None
                    else:
                        # Caso chegue um PONG inesperado (debug da ESP)
                        print(f"[ESP DEBUG] {line}")
                    continue

                # 
                # 2. Relatórios longos (gerados a cada 60s)
                # 
                if line.startswith("=== REPORT TCP"):
                    print("\n" + "="*70)
                    print("[RELATÓRIO TCP RECEBIDO]")
                    print(line)
                    print("="*70 + "\n")
                    continue

                # 
                # 3. Logs JSON periódicos
                # 
                try:
                    # Exemplo de JSON vindo da ESP32:
                    # {"seq": 12, "rpm": 124.3, "count": 1, "estop": false}
                    j = json.loads(line)
                    print(f"[LOG {j.get('seq')}] RPM: {j.get('rpm'):.1f} | Peças: {j.get('count')} | E-Stop: {j.get('estop')}")
                except json.JSONDecodeError:
                    # Caso a linha não seja JSON válido, imprime como debug
                    print(f"[ESP DEBUG] {line}")

        except socket.timeout:
            # Timeout normal (sem dados novos)
            continue
        except Exception as e:
            # Qualquer outro erro de socket
            if not stop_thread:
                print(f"[ERRO TCP] {e}")
            break


# 
#  ENVIO DE COMANDOS PARA A ESP32
# 
def send_command(sock, cmd):
    """Envia comandos de texto para a ESP32 (terminados por '\n')."""
    try:
        sock.sendall((cmd + "\n").encode('utf-8'))  # Envia comando completo
        print(f"[PC -> ESP] Enviado: {cmd}")
    except Exception as e:
        print(f"[ERRO] Falha ao enviar comando: {e}")


#
#  PROGRAMA PRINCIPAL
# 
def main():
    global stop_thread, last_ping_time
    sock = None  # Variável de socket global da conexão TCP

    try:
        
        # Conecta ao servidor TCP da ESP32
        sock = socket.create_connection((ESP32_IP, TCP_PORT), timeout=5)
        sock.settimeout(RECV_TIMEOUT)  # Timeout curto para leitura
        print(f"[TCP] Conectado à ESP32 {ESP32_IP}:{TCP_PORT}")

        # Cria thread de recepção paralela
        threading.Thread(target=recv_tcp, args=(sock,), daemon=True).start()

        # Loop interativo para envio de comandos
        
        while not stop_thread:
            cmd_input = input("\n1=SORT_ACT | 2=SAFETY_ON | 3=PING | 0=Sair\nComando: ").strip()

            if cmd_input == "0":
                stop_thread = True
                break
            elif cmd_input == "1":
                send_command(sock, "SORT_ACT")   # Aciona atuador
            elif cmd_input == "2":
                send_command(sock, "SAFETY_ON")  # Liga modo de segurança
            elif cmd_input == "3":
                # Marca tempo do envio para medir RTT
                last_ping_time = time.time()
                send_command(sock, "PING")
            else:
                print("Comando inválido")

            time.sleep(0.1)  # Pequeno atraso para não saturar o loop

    except Exception as e:
        print(f"[ERRO] Conexão TCP falhou: {e}")

    finally:
        # Encerra conexão e limpa recursos
        
        stop_thread = True
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except:
                pass
            sock.close()
        print("[TCP] Conexão encerrada.")



#  PONTO DE ENTRADA DO SCRIPT

if __name__ == "__main__":
    main()
