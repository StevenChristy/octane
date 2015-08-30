#include <thread>
#include <vector>
#include <iostream>
using namespace std;

#include "octane.cpp"

const int MAX_THREADS = 2000;

class stuff_class 
{
public:
    int a;
    string b;
    float c;
};


int main()
{
    auto clock_start = chrono::high_resolution_clock::now();
    vector< shared_ptr<thread> > Threads;
    for ( int x = 0; x < MAX_THREADS; x++ )
        Threads.push_back(shared_ptr<thread>(new thread([]() {
            vector< shared_ptr<stuff_class> > stuff;
            for ( int x = 0; x < MAX_THREADS*10; x++ ) {
                stuff.push_back(shared_ptr<stuff_class>(new stuff_class()));
            }
            // Free stuff in threads, this should create con
            auto contend = [&stuff]( int cnt, int start, int skip) {
                for ( int x = start; x < cnt; x += skip ) {
                    stuff[x].reset();
                }
            };
            
            thread contender1( contend, stuff.size(), 0, 2);
            thread contender2( contend, stuff.size(), 1, 2);

            vector< shared_ptr<stuff_class> > pressure;
            for ( int x = 0; x < MAX_THREADS*10; x++ ) {
                pressure.push_back(shared_ptr<stuff_class>(new stuff_class()));
            }
            
            contender1.join();           
            contender2.join();
        })));
   
    for ( auto &threadptr : Threads )
        threadptr->join();

    auto clock_end = chrono::high_resolution_clock::now();
    
    cout << "Time elapsed " << chrono::duration_cast<chrono::milliseconds>(clock_end - clock_start).count() << "ms" << endl ;
    return EXIT_SUCCESS;
}