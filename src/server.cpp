#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <iomanip>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

using namespace std;

#define SERVER_PORT 8080
#define BUFFER_SIZE 1024
#define THREAD_POOL_SIZE 5

// Простая контрольная сумма (CRC32)
unsigned int calculateChecksum(const string& filename, long startOffset, long endOffset) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) {
        cerr << "Ошибка открытия файла: " << filename << endl;
        return 0;
    }

    unsigned int checksum = 0;
    char buffer[1024];
    file.seekg(startOffset, ios::beg);
    while (file.tellg() < endOffset && file.read(buffer, sizeof(buffer))) {
        for (int i = 0; i < file.gcount(); i++) {
            checksum += (unsigned char)buffer[i];
        }
    }

    return checksum;
}

bool validateChecksum(const string& filename, unsigned int checksum, long startOffset, long endOffset) {
    return calculateChecksum(filename, startOffset, endOffset) == checksum;
}

mutex mtx;
condition_variable cv;
queue<int> clientQueue;
atomic<bool> stop(false);

void handleClient(int clientSocket, const string& targetFolder) {
    char buffer[BUFFER_SIZE];

    while (true) {
        // Отправляем сигнал готовности к приему файла
        string readySignal = "READY";
        if (send(clientSocket, readySignal.c_str(), readySignal.length(), 0) != readySignal.length()) {
            close(clientSocket);
            break; // Заканчиваем цикл приема файлов
        }

        // Получаем имя файла от клиента
        char filenameBuffer[1024];
        int bytesReceived = recv(clientSocket, filenameBuffer, sizeof(filenameBuffer), 0);
        if (bytesReceived <= 0) {
            cerr << "Ошибка получения имени файла" << endl;
            close(clientSocket);
            break; // Заканчиваем цикл приема файлов
        }
        string filename(filenameBuffer, bytesReceived);

        cout << "Получено имя файла: " << filename << endl;

        // Получаем размер файла от клиента
        char sizeBuffer[1024];
        bytesReceived = recv(clientSocket, sizeBuffer, sizeof(sizeBuffer), 0);
        if (bytesReceived <= 0) {
            cerr << "Ошибка получения размера файла" << endl;
            close(clientSocket);
            break; // Заканчиваем цикл приема файлов
        }

        long fileSize = atol(sizeBuffer);
        cout << "Получен размер файла: " << fileSize << endl;

        // Получаем контрольную сумму от клиента
        char checksumBuffer[256];
        bytesReceived = recv(clientSocket, checksumBuffer, sizeof(checksumBuffer), 0);
        if (bytesReceived <= 0) {
            cerr << "Ошибка получения контрольной суммы" << endl;
            close(clientSocket);
            break; // Заканчиваем цикл приема файлов
        }

        unsigned int checksum = static_cast<unsigned int>(atol(checksumBuffer));

        // Проверяем, нужно ли докачивать файл
        long receivedBytes = 0;
        string fullPath = targetFolder + "/" + filename;
        if (access(fullPath.c_str(), F_OK) != -1) {
            cout << "Файл существует, проверяем размер" << endl;
            int fileDescriptor = open(fullPath.c_str(), O_RDWR);
            if (fileDescriptor == -1) {
                cerr << "Ошибка открытия файла: " << fullPath << endl;
                close(clientSocket);
                break; // Заканчиваем цикл приема файлов
            }
            receivedBytes = lseek(fileDescriptor, 0, SEEK_END); // Получаем размер уже существующей части файла
            close(fileDescriptor);
            if (receivedBytes <= fileSize) {
                cout << "Пропускаем " << receivedBytes << " байт" << endl;
                // Отправляем размер файла на сервер
                string sizeStr = to_string(receivedBytes);
                if (send(clientSocket, sizeStr.c_str(), sizeStr.length(), 0) != sizeStr.length()) {
                    cerr << "Ошибка отправки размера файла" << endl;
                    close(clientSocket);
                    break; // Заканчиваем цикл приема файлов
                }
            } else {
                cout << "Файл уже полностью загружен" << endl;
                continue;
            }
        } else {
            // Отправляем размер файла на сервер (0, т.к. файл не существует)
            string sizeStr = to_string(0);
            if (send(clientSocket, sizeStr.c_str(), sizeStr.length(), 0) != sizeStr.length()) {
                cerr << "Ошибка отправки размера файла" << endl;
                close(clientSocket);
                break; // Заканчиваем цикл приема файлов
            }
        }

        // Открываем файл для записи (в бинарном режиме)
        int fileDescriptor = open(fullPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); // O_APPEND добавляет в конец файла
        if (fileDescriptor == -1) {
            cerr << "Ошибка открытия файла: " << fullPath << endl;
            close(clientSocket);
            break; // Заканчиваем цикл приема файлов
        }

        cout << "Начало приема файла " << filename << "..." << endl;
        char buffer[256];
        while (receivedBytes < fileSize) {
            // Получаем часть файла (с MSG_WAITALL)
            bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
            if (bytesReceived <= 0) {
                close(clientSocket);
                break; // Заканчиваем цикл приема файлов
            }

            // Записываем полученные данные в файл
            int bytesWritten = write(fileDescriptor, buffer, bytesReceived);
            if (bytesWritten != bytesReceived) {
                cerr << "Ошибка записи в файл" << endl;
                close(clientSocket);
                break; // Заканчиваем цикл приема файлов
            }

            receivedBytes += bytesWritten;
        }

        // Закрываем файл
        close(fileDescriptor);

        // Вычисляем контрольную сумму всего файла
        unsigned int checksumThis = calculateChecksum(fullPath, 0, fileSize);

        cout << "Файл " << filename << " получен." << endl;

        // Проверяем контрольную сумму
        if (checksumThis == checksum) {
            cout << "Контрольная сумма верна. Успешная передача файла " << filename << endl;
        } else {
            cout << "Ошибка проверки контрольной суммы. Файл " << filename << " поврежден." << endl;
        }
    }

    close(clientSocket);
}

void workerThread(const string& targetFolder) {
    while (true) {
        int clientSocket;
        {
            unique_lock<mutex> lock(mtx);
            cv.wait(lock, []{ return !clientQueue.empty() || stop; });
            if (stop && clientQueue.empty()) {
                return;
            }
            clientSocket = clientQueue.front();
            clientQueue.pop();
        }
        handleClient(clientSocket, targetFolder);
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Неверный формат вызова: " << argv[0] << " <порт> <папка>" << endl;
        return 1;
    }

    int port = atoi(argv[1]);
    string targetFolder = argv[2];

    int serverSocket;
    struct sockaddr_in serverAddress, clientAddress;
    socklen_t clientAddressSize = sizeof(clientAddress);

    // Создание сокета
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        cerr << "Ошибка создания сокета" << endl;
        return 1;
    }

    // Заполнение структуры адреса
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(port);

    // Связывание сокета с адресом
    if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        cerr << "Ошибка связывания сокета" << endl;
        return 1;
    }

    // Слушаем подключения
    if (listen(serverSocket, 5) < 0) {
        cerr << "Ошибка прослушивания подключения" << endl;
        return 1;
    }

    cout << "Сервер запущен на порту " << port << endl;

    // Создание пула потоков
    vector<thread> threads;
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        threads.emplace_back(workerThread, targetFolder);
    }

    while (true) {
        // Принимаем соединение
        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientAddressSize);
        if (clientSocket < 0) {
            cerr << "Ошибка принятия соединения" << endl;
            continue;
        }

        // Помещаем сокет клиента в очередь
        {
            lock_guard<mutex> lock(mtx);
            clientQueue.push(clientSocket);
        }
        cv.notify_one();
    }

    // Остановка потоков
    {
        lock_guard<mutex> lock(mtx);
        stop = true;
    }
    cv.notify_all();

    for (auto& th : threads) {
        if (th.joinable()) {
            th.join();
        }
    }

    close(serverSocket);
    return 0;
}