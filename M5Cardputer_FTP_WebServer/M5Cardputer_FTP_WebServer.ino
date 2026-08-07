/* ESP32FtpServer by Amauri Bueno*/

#include <M5Cardputer.h>
#include <WiFi.h>
#include <SD.h>
#include <ESP32FtpServer.h> // Biblioteca nativa com suporte ao SD no ESP32-S3
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

// Configuraçoes de rede
const char* ssid = "Archenar";
const char* password = "PASS1234567890000";
const char* hostname = "aldebaram";

// Instancia o servidor FTP
FtpServer ftpSrv;

// Servidor Web
WebServer server(80);

// Manipuador para multiprocessamento
TaskHandle_t FTPTaskHandle = NULL;

// Task rodando no Core 0 em segundo plano para o FTP
void ftpTask(void * pvParameters) {
  // Inicia o FTP APENAS UMA VEZ no escopo da Task
  ftpSrv.begin("esp32", "123456");

  for (;;) {
    ftpSrv.handleFTP(); // Processa os pacotes recebidos
    
    // Use pdMS_TO_TICKS(10) para garantir que o tempo do RTOS seja correto no ESP32-S3
    // Isso dá o tempo exato para a biblioteca esvaziar o buffer e fechar o arquivo no SD
    vTaskDelay(pdMS_TO_TICKS(10)); 
  }
}

// Inicializa o M5Cardputer (Display e Teclado)
void startConfiguration(){
  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
}

// Inicializa displau
void startDisplay(){
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.println("Iniciando...");
}

void startWifi(){
  // Conecta ao Wi-Fi e desativa o modo de economia de energia (Sleep)
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setHostname(hostname);

  // ESSENCIAL para o FTP não desconectar
  WiFi.setSleep(false); 

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi Conectado!");
}

// inicializa Storage
void startStorage(){
  SPI.begin(40, 39, 14, 12); 
  if (!SD.begin(12, SPI, 10000000)) {
    Serial.println("ERRO: Falha ao montar o Cartão SD!");
    M5Cardputer.Display.setTextColor(RED);
    M5Cardputer.Display.println("Erro SD!");
  } else {
    Serial.println("Cartão SD montado com sucesso!");
    M5Cardputer.Display.setTextColor(GREEN);
    M5Cardputer.Display.println("SD OK!");
  }
}

void startDNS(){
  // Inicializa o servico de DNS
  if(MDNS.begin(hostname)){
    Serial.println("Serviço de DNS iniciado");
  }else{
    Serial.println("Não foi possivel iniciar o serviço de DNS");
  }
}

//Cria a Task do FTP rodando isolada no CORE 0
void startThreads(){
  xTaskCreatePinnedToCore(
    ftpTask,
    "FTPTask",
    32768,            // 16KB de RAM dedicada
    NULL,
    1,
    &FTPTaskHandle,
    0                 // Core 0
  );
}

// Função auxiliar para entregar arquivos do Cartão SD (HTML, CSS, JS, etc)
bool loadFileFromSD(String path, String contentType) {
  if (SD.exists(path)) {
    File file = SD.open(path, "r");
    server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

// Inicializa Web Server
void startWebserver(){

  // Rota genérica: captura qualquer arquivo requisitado na raiz ou em subpastas
  server.onNotFound([]() {
    String path = server.uri();

    // Se a URL tiver parâmetros como ?v=1, limpa para pegar só o caminho do arquivo
    int queryIndex = path.indexOf('?');
    if (queryIndex != -1) {
      path = path.substring(0, queryIndex);
    }

    // Se for só a raiz "/", carrega o index.html por padrão
    if (path == "/") {
      path = "/index.html";
    }

    // Determina o Content-Type correto com base na extensão do arquivo (.js, .css, .png, etc)
    String contentType = getContentType(path);

    // Tenta carregar QUALQUER arquivo requisitado do SD (qualquer subpasta/nome)
    if (!loadFileFromSD(path, contentType)) {
      server.send(404, "text/plain", "404: Arquivo nao encontrado no SD");
    }
  });

  // Endpoint para listar o conteúdo do SD usando ArduinoJson
  server.on("/api/list", HTTP_GET, []() {
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
      server.send(500, "application/json", "{\"error\":\"Falha ao abrir o diretorio raiz do SD\"}");
      return;
    }

    // Usando JsonDocument dinâmico (ArduinoJson v7)
    JsonDocument doc;
    JsonArray rootArray = doc.to<JsonArray>();

    // Popula o JSON recursivamente
    listDirRecursiveJson(root, rootArray);
    root.close();

    // Serializa para uma String e envia via WebServer
    String output;
    serializeJson(doc, output);

    server.send(200, "application/json", output);
  });

   
   // Endpoint que simula uma listagem FTP/Apache direto no navegador sem arquivo HTML no SD
  server.on("/files", HTTP_GET, []() {
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
      server.send(500, "text/plain", "Erro ao abrir o cartao SD");
      return;
    }

    String html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Index of /</title></head><body>";
    html += "<h2>Index of /</h2><hr><table style=\"width:100%; font-family:monospace;\">";
    html += "<tr><th align=\"left\">Name</th><th align=\"right\">Size</th></tr>";

    // CORREÇÃO AQUI: Passamos "" (string vazia) como o 4º parâmetro (caminho inicial)
    generateFtpDirectoryListing(root, html, 0, "");

    html += "</table><hr><em>M5Cardputer Server</em></body></html>";
    root.close();

    server.send(200, "text/html", html);
  });

  
 // Endpoint responsável por baixar o arquivo do SD
  server.on("/download", HTTP_GET, []() {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Parametro 'file' ausente");
      return;
    }

    String filePath = server.arg("file");

    if (SD.exists(filePath)) {
      File file = SD.open(filePath, "r");
      
      // MELHORIA: Extrai apenas o nome do arquivo (tira as pastas do caminho)
      int lastSlash = filePath.lastIndexOf('/');
      String fileName = (lastSlash >= 0) ? filePath.substring(lastSlash + 1) : filePath;
      
      // Adiciona o cabeçalho que garante que o navegador vai salvar com o nome original correto
      server.sendHeader("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
      
      // O tipo genérico "application/octet-stream" força o navegador a baixar o arquivo
      server.streamFile(file, "application/octet-stream");
      file.close();
    } else {
      server.send(404, "text/plain", "404: Arquivo nao encontrado no SD");
    }
  });


  server.begin();
}


/// Função recursiva atualizada com links de download e caminhos completos
void generateFtpDirectoryListing(File dir, String &htmlOutput, int level, String currentPath) {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      break;
    }

    String fileName = String(entry.name());
    
    // Identação visual por nível de pasta
    String indent = "";
    for (int i = 0; i < level; i++) {
      indent += "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;"; 
    }

    // Constrói o caminho completo juntando o caminho atual com o nome do arquivo
    String filePath = currentPath + "/" + fileName;
    
    // Prevenção: remove barras duplas caso a raiz venha como "/"
    filePath.replace("//", "/");

    if (entry.isDirectory()) {
      htmlOutput += "<tr><td>" + indent + "📁 <strong>[" + fileName + "]</strong></td><td>-</td></tr>";
      
      // Quando entra na subpasta, passa o 'filePath' atual para a próxima recursão
      generateFtpDirectoryListing(entry, htmlOutput, level + 1, filePath);
    } else {
      long fileSize = entry.size();
      
      // Agora o link aponta para o caminho completo correto
      htmlOutput += "<tr><td>" + indent + "📄 <a href=\"/download?file=" + filePath + "\" download>" + fileName + "</a></td><td align=\"right\">" + String(fileSize) + " bytes</td></tr>";
    }

    entry.close();
  }
}



String getContentType(String filename) {
  if (filename.endsWith(".html") || filename.endsWith(".htm")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".xml")) return "text/xml";
  else if (filename.endsWith(".pdf")) return "application/x-pdf";
  else if (filename.endsWith(".zip")) return "application/x-zip";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

// Exibe mensagens
void setMessage(){
  M5Cardputer.Display.setTextColor(WHITE);
  M5Cardputer.Display.println("FTP no SD:");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.println(WiFi.localIP());
}

// Função auxiliar recursiva usando ArduinoJson
void listDirRecursiveJson(File dir, JsonArray &jsonArray) {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      break; // Fim dos arquivos deste diretório
    }

    JsonObject obj = jsonArray.add<JsonObject>();
    obj["name"] = String(entry.name());

    if (entry.isDirectory()) {
      obj["isDirectory"] = true;
      obj["size"] = 0;
      JsonArray children = obj["children"].to<JsonArray>();
      listDirRecursiveJson(entry, children); // Chamada recursiva para subpasta
    } else {
      obj["isDirectory"] = false;
      obj["size"] = entry.size();
    }

    entry.close();
  }
}

// Função para contar arquivos recursivamente e verificar o uso do SD
void getSDStats(File dir, int &fileCount, long &usedBytes) {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      break;
    }

    if (entry.isDirectory()) {
      // Se for pasta, entra nela recursivamente
      getSDStats(entry, fileCount, usedBytes);
    } else {
      // Se for arquivo, incrementa a contagem e soma o tamanho
      fileCount++;
      usedBytes += entry.size();
    }
    entry.close();
  }
}

void showSDInfoOnScreen() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setCursor(0, 0);
  
  // Título com destaque
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(CYAN);
  M5Cardputer.Display.println("SD STATUS");
  M5Cardputer.Display.println(); // Linha em branco menor

  // Abre a raiz para varredura
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    M5Cardputer.Display.setTextColor(RED);
    M5Cardputer.Display.println("Erro SD!");
    return;
  }

  int totalFiles = 0;
  long usedSpace = 0;

  getSDStats(root, totalFiles, usedSpace);
  root.close();

  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytesSD = SD.usedBytes();
  
  float usedMB = (float)usedBytesSD / (1024 * 1024);
  float percentage = (totalBytes > 0) ? ((float)usedBytesSD / totalBytes) * 100.0 : 0.0;

  // Mantém o tamanho 2 para os dados principais ficarem bem legíveis
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(WHITE);
  M5Cardputer.Display.printf("Arq: %d\n", totalFiles);
  M5Cardputer.Display.printf("Uso: %.1fMB\n", usedMB);
  M5Cardputer.Display.printf("Pct: %.1f%%\n", percentage);
  M5Cardputer.Display.println();

  // --- GRÁFICO DE BARRAS ---
  int barX = 5;
  int barY = M5Cardputer.Display.getCursorY();
  int barWidth = 230;
  int barHeight = 10;

  M5Cardputer.Display.drawRect(barX, barY, barWidth, barHeight, WHITE);

  int filledWidth = (int)((barWidth - 2) * (percentage / 100.0));
  if (filledWidth > (barWidth - 2)) filledWidth = barWidth - 2;
  if (filledWidth < 0) filledWidth = 0;

  uint16_t barColor = GREEN;
  if (percentage >= 70.0 && percentage < 90.0) {
    barColor = YELLOW;
  } else if (percentage >= 90.0) {
    barColor = RED;
  }

  M5Cardputer.Display.fillRect(barX + 1, barY + 1, filledWidth, barHeight - 2, barColor);

  // Rodapé com o IP usando fonte menor (tamanho 1) para não estourar a tela embaixo
  M5Cardputer.Display.setCursor(0, barY + barHeight + 6);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(GREEN);
  M5Cardputer.Display.println(WiFi.localIP().toString());
}

void setup() {
  Serial.begin(115200);

  startConfiguration();
  startDisplay();
  startWifi();
  startDNS();
  startStorage();
  startThreads();
  startWebserver();
  setMessage();
}

void loop() {
  // atualiza as configuracoes do dispositovo
  M5Cardputer.update();
  
  // Escuta as requisiçoes do servidor web
  server.handleClient();

  // Verifica se houve mudança no estado do teclado
  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      // Pega o estado atual do teclado
      auto status = M5Cardputer.Keyboard.keysState();
      
      // Verifica se a tecla Enter foi pressionada
      if (status.enter) {
        showSDInfoOnScreen(); // Recalcula e exibe os dados do SD na tela
      }
    }
  }

  // Tempo de descanso
  delay(10);
}