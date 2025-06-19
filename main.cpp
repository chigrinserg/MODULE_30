#include <iostream>
#include <future>
#include <chrono>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <functional>
#include <algorithm>

using namespace std;
const int THRESHOLD = 1000; // Минимальная длина сегмента для параллельной сор-тировки
mutex mtx;

class ThreadPool // Класс пула потоков
{
public:
    ThreadPool() : m_thread_count(max<int>(1, thread::hardware_concurrency())) {}
    ~ThreadPool() { stop(); }

    void start();               // Запускает пул
    void stop();                // Останавливает пул

    template<typename Func>
    void pushTask(Func&& f, int arr[], int low, int high); // Отправляет задание
    void threadFunc();           // Выполняется каждым потоком
    size_t getActiveTasksCount() const
    {
        return active_tasks.load();// Получаем кол-во активных задач
    }

private:
    vector<thread> m_threads;                  // Потоки
    mutex m_locker;                            // Мьютекс для защиты очереди
    queue<function<void()>> m_task_queue;      // Очередь задач
    condition_variable m_event_holder;         // Условная переменная для ожидания
    atomic<bool> m_work;                       // Активность пула
    int m_thread_count{};                      // Число потоков
    atomic<size_t> active_tasks{ 0 };          // Кол-во активных задач
};

void ThreadPool::start() //Запуск потоков
{
    m_work = true;
    for (int i = 0; i < m_thread_count; i++)
    {
        m_threads.emplace_back([this] { threadFunc(); });
    }
}

void ThreadPool::stop() //Остановка потоков
{
    m_work.store(false);
    m_event_holder.notify_all();
    for (auto& t : m_threads)
    {
        if (t.joinable())
            t.join();
    }
}

template<typename Func>
void ThreadPool::pushTask(Func&& f, int arr[], int low, int high) //Загрузка задач в очередь
{
    lock_guard<mutex> l(m_locker);
    auto lambda = [f = std::move(f), arr, low, high, this]() mutable {
        f(arr, low, high);
        this->active_tasks--;
    };
    this->active_tasks++;
    m_task_queue.push(lambda);
    m_event_holder.notify_one();
}

void ThreadPool::threadFunc() //Основная функция потока
{
    while (true)
    {
        function<void()> task_to_do;
        {
            unique_lock<mutex> l(m_locker);

            if (!m_work.load()) {
                return;
            }
            if (m_task_queue.empty())
                m_event_holder.wait(l, [&]()
                    { return !m_task_queue.empty() || !m_work.load(); });

            if (!m_task_queue.empty())
            {
                task_to_do = move(m_task_queue.front());
                m_task_queue.pop();
            }
        }
        if (task_to_do)
            task_to_do();
    }
}

void swap(int& a, int& b) { // Простой обмен элементов
    int temp = a;
    a = b;
    b = temp;
}

int partition(int arr[], int low, int high) { // Разделение массива
    int pivot = arr[high]; // Опорный элемент
    int i = low - 1;
    for (int j = low; j <= high - 1; ++j) {
        if (arr[j] < pivot) {
            i++;
            swap(arr[i], arr[j]); // Меняем местами меньшие элементы
        }
    }
    swap(arr[i + 1], arr[high]); // Перемещаем опорный элемент
    return i + 1;
}

void quickSort(int arr[], int low, int high)// Обычная быстрая сортировка
{
    if (low >= high) return;
    int pi = partition(arr, low, high);
    quickSort(arr, low, pi - 1);
    quickSort(arr, pi + 1, high);
}

// Параллельная быстрая сортировка
void parallelQuickSort(ThreadPool& tpool, int arr[], int low, int high)
{
    if (low >= high) return;
    int pi = partition(arr, low, high);

    if ((pi - low) > THRESHOLD)  // Только большие сегменты идут на параллельную сортировку
    {
        tpool.pushTask([&](int arr[], int low, int high) { parallelQuickSort(tpool, arr, low, high); },
            arr, low, pi - 1); //Загузка в очередь первой половины массива
    }
    else
    {
        quickSort(arr, low, pi - 1); // Последовательная сортировка малых частей
    }
    if ((high - pi) > THRESHOLD)
    {
        tpool.pushTask([&](int arr[], int low, int high) { parallelQuickSort(tpool, arr, low, high); },
            arr, pi + 1, high); //Загрузка в очередь второй половины массива
    }
    else
    {
        quickSort(arr, pi + 1, high); // Последовательная сортировка малых частей
    }
}

class RequestHandler // Класс - обработчик запросов
{
public:
    RequestHandler() { m_tpool.start(); }
    ~RequestHandler() { m_tpool.stop(); }

    template<typename Func>
    void pushRequest(Func&& f, int arr[], int low, int high)
    {
        m_tpool.pushTask(std::forward<Func>(f), arr, low, high);
    }
    ThreadPool m_tpool;
};

RequestHandler rh;

void taskFunc(int arr[], int low, int high) // Основная задача
{
    parallelQuickSort(rh.m_tpool, arr, low, high); // Используем параллельную сортировку
}

int main()
{
    setlocale(LC_ALL, "Rus");
    srand(time(nullptr));

    const int size = 1000000; //Заданный размер массива
    int* array = new int[size] {}; //Объявляем динамический массив
    for (int i = 0; i < size; i++) //Заполняем массив случайными значениями
    {
        array[i] = rand() % (size * 2);
    }

    auto start = std::chrono::high_resolution_clock::now();
    rh.pushRequest(taskFunc, array, 0, size - 1);

    while (rh.m_tpool.getActiveTasksCount() > 0)// Ждем завершения всех задач
    {
        this_thread::yield();
    }
    auto finish = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = finish - start;
    std::cout << "Время выполнения многопоточной соритровки: " << elapsed.count() << " сек." << std::endl;

    for (int i = 0; i < size; i++)
    {
        // cout << array[i] << " "; // Вывод отсортировыанного массива отключен, чтобы не тормозило вывод
    }

    if (std::is_sorted(array, array + size)) {
        std::cout << "Массив успешно отсортирован!" << std::endl;
    }
    else {
        std::cout << "Массив не отсортирован!" << std::endl;
    }
    std::cout << endl;
    delete[] array;
    return 0;
}
