import socket
import threading
import json
import time
# tcp_c
ESP32_IP = "192.168.7.7"
TCP_PORT = 5000
RECV_TIMEOUT = 1.0
stop_thread = False

# Variável global para RTT
last_ping_time = None

def recv_tcp(sock):
    """Recebe logs JSON e relatórios TCP."""
    global stop_thread, last_ping_time
    buffer = ""

    while not stop_thread:
        try:
            data = sock.recv(4096)
            if not data:
                print("[TCP] Conexão encerrada pelo ESP32")
                stop_thread = True
                break

            buffer += data.decode("utf-8", errors="ignore")
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                line = line.strip()
                if not line:
                    continue

                # Detecta resposta de PING
                if line.startswith("PONG"):
                    if last_ping_time is not None:
                        rtt = (time.time() - last_ping_time) * 1000
                        print(f"[PING TCP] Recebido '{line}' | RTT ≈ {rtt:.2f} ms")
                        last_ping_time = None
                    else:
                        print(f"[ESP DEBUG] {line}")
                    continue

                # Relatórios longos
                if line.startswith("=== REPORT TCP"):
                    print("\n" + "="*70)
                    print("[RELATÓRIO TCP RECEBIDO]")
                    print(line)
                    print("="*70 + "\n")
                    continue

                # Logs JSON periódicos
                try:
                    j = json.loads(line)
                    print(f"[LOG {j.get('seq')}] RPM: {j.get('rpm'):.1f} | Peças: {j.get('count')} | E-Stop: {j.get('estop')}")
                except json.JSONDecodeError:
                    print(f"[ESP DEBUG] {line}")

        except socket.timeout:
            continue
        except Exception as e:
            if not stop_thread:
                print(f"[ERRO TCP] {e}")
            break


def send_command(sock, cmd):
    """Envia comandos TCP (com quebra de linha obrigatória)."""
    try:
        sock.sendall((cmd + "\n").encode('utf-8'))
        print(f"[PC -> ESP] Enviado: {cmd}")
    except Exception as e:
        print(f"[ERRO] Falha ao enviar comando: {e}")


def main():
    global stop_thread, last_ping_time
    sock = None

    try:
        sock = socket.create_connection((ESP32_IP, TCP_PORT), timeout=5)
        sock.settimeout(RECV_TIMEOUT)
        print(f"[TCP] Conectado à ESP32 {ESP32_IP}:{TCP_PORT}")

        threading.Thread(target=recv_tcp, args=(sock,), daemon=True).start()

        while not stop_thread:
            cmd_input = input("\n1=SORT_ACT | 2=SAFETY_ON | 3=PING | 0=Sair\nComando: ").strip()

            if cmd_input == "0":
                stop_thread = True
                break
            elif cmd_input == "1":
                send_command(sock, "SORT_ACT")
            elif cmd_input == "2":
                send_command(sock, "SAFETY_ON")
            elif cmd_input == "3":
                last_ping_time = time.time()
                send_command(sock, "PING")
            else:
                print("Comando inválido")
            time.sleep(0.1)

    except Exception as e:
        print(f"[ERRO] Conexão TCP falhou: {e}")
    finally:
        stop_thread = True
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except:
                pass
            sock.close()
        print("[TCP] Conexão encerrada.")


if __name__ == "__main__":
    main()
