#include <iostream>
#include <cstdio>
#include <cstdint>
#include <string>
#include <cstring>
#include <functional>
#include <chrono>
#include <malloc.h>

#include "context.h"

using coro_func_t=std::function<void()>;

#define DFT_STACK_PER_CORO (1<<12)
#define CACHE_ALIGNED __attribute__((aligned(64)))
class Coro_Scheduler;

class Coroutine{
public:
  friend class Coro_Scheduler;
  friend class CoroutineQueue;

  inline void reset(coro_func_t func){
    sch_ = NULL;
    this->func_ = func;
    //memset(stack_,0,sizeof(stack_));
    memset(stack_, 0, DFT_STACK_PER_CORO);
  }

  ~Coroutine(){
    free(stack_);
    free(ctx_);
  }

  Coroutine(){
    //memset(stack_,0,sizeof(stack_));
    ctx_ = (ucontext_t*)malloc( sizeof(ucontext_t) );
    stack_ = (char*) malloc( DFT_STACK_PER_CORO );
    sch_ = NULL;
    func_ = NULL;
    father_ = NULL;
    next_ = NULL;
    status_ = 0;
  }

private:
  // try to align stack
  //uint64_t stack_[DFT_STACK_PER_CORO];
  char* stack_;

  //one cache line
  Coro_Scheduler* sch_;
  coro_func_t func_;
  ucontext_t* ctx_;
  ucontext_t* father_;

  
  Coroutine *next_;
  union{
    struct{
      int idle_ : 1;
      int ready_ : 1;
      int running_ : 1;
      int suspended_ : 1;
    };
    int8_t status_;
  };

};

class CoroutineQueue{
public:

  CoroutineQueue()
    : head_(NULL)
    , tail_(NULL)
    , size_(0){}

  inline void enqueue(Coroutine * c){
    if(NULL == head_) head_ = c;
    else tail_->next_ = c;
    tail_ = c;
    c->next_ = NULL;
    size_++;
  }

  inline Coroutine* dequeue(){
    Coroutine* ret = head_;
    if(NULL != ret){
      head_ = head_->next_;

      //prefetch
      //if(NULL != ret->next_ ){
        //__builtin_prefetch( ret->next_, 1, 3);
        //__builtin_prefetch( (char*)(ret->next_->ctx_.mc.rsp)-64, 1, 3);
        //__builtin_prefetch( (char*)(ret->next_->ctx_.mc.rsp)-128, 1, 3);
      //}

      ret->next_ = NULL;
      size_ -- ;

    }else{
      tail_ = NULL;
    }
    return ret;
  }

  inline Coroutine* front(){
    return head_;
  }
  inline uint64_t size(){
    return size_;
  }

private:
  Coroutine* head_;
  Coroutine* tail_;
  uint64_t size_;
};


class Coro_Scheduler{
public:
  Coro_Scheduler() : running_(true), executant_(NULL) {}

  inline Coroutine * get_cur_coro(){
    return executant_;
  }

  inline size_t depth(){
    return candidates_.size();
  }

  void await(); 

  Coroutine * _spawn_coro(coro_func_t func);
  void _delete_coro(Coroutine * coro);
  Coroutine * _next_coro(); 

  void coroutine_create( coro_func_t func );
  void coroutine_yeild();
  static void tramp(void * arg);

private:
  ucontext_t main_;
  Coroutine * executant_;

  /* just a flag to see if current coroutine is Coro_Scheduler */
  bool running_;
 
  CoroutineQueue candidates_;
};

Coroutine * Coro_Scheduler::_spawn_coro(coro_func_t func){
  Coroutine * coro = new Coroutine;
  coro->reset(func);
  return coro;
}

void Coro_Scheduler::_delete_coro(Coroutine * coro){
  free(coro);
  coro = NULL;
}

Coroutine* 
Coro_Scheduler::_next_coro(){
  return candidates_.dequeue();
}

void Coro_Scheduler::await(){
  for(;;){
    Coroutine * next_coro = _next_coro();
    if( next_coro == NULL ) break;
    next_coro->running_ = 1;
    next_coro->ready_ = 0;
    executant_ = next_coro;
    running_ = false;
    swapcontext(&main_, next_coro->ctx_);
    running_ = true;
  }
}

void Coro_Scheduler::coroutine_create( coro_func_t func ){
  Coroutine * coro = _spawn_coro( func );
  //getcontext(&(coro->ctx_));
  coro->ctx_->uc_stack.ss_sp = (void *)coro->stack_;
  coro->ctx_->uc_stack.ss_size = DFT_STACK_PER_CORO;
  coro->ctx_->uc_stack.ss_flags = 0;
  coro->ctx_->uc_link = &main_;
  coro->ready_ = 1;
  coro->sch_ = this;

  if( running_ ){
    coro->father_ = &main_;
  }else{
    coro->father_ = executant_->ctx_;
  }

  //printf("adadfasd\n");
  makecontext(coro->ctx_, (void (*)(void*))(tramp), reinterpret_cast<void*>(coro));
  //printf("adadfasd\n");
  swapcontext(coro->father_, coro->ctx_);
  //printf("adadfasd\n");
  candidates_.enqueue( coro );
}

void Coro_Scheduler::coroutine_yeild(){
  Coroutine* next_coro = _next_coro();

  if( NULL == next_coro ) return;

  Coroutine * coro = executant_;
  /* put it back */
  candidates_.enqueue( coro );

  executant_ = next_coro;

  coro->running_ = 0;
  coro->ready_ = 1;
  next_coro->ready_ = 0;
  next_coro->running_ = 1;

  swapcontext( coro->ctx_, next_coro->ctx_ ) ;
}

void Coro_Scheduler::tramp(void* arg){

  Coroutine* cur = reinterpret_cast<Coroutine*>(arg);
  swapcontext( cur->ctx_, cur->father_ );

  // this time it is a real swapcontext
  cur->func_();
  // don't forget to return the cpu to your father
  cur->running_ = 0;
  cur->idle_ = 1;
  swapcontext( cur->ctx_, cur->father_);
}

inline auto time() -> decltype(std::chrono::high_resolution_clock::now()){
  return std::chrono::high_resolution_clock::now();
}

template<typename T>
inline uint64_t diff(T start, T end){
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
}

int main(){
  const int coro_num = 1<<3;
  const int switch_time = 10;
  Coro_Scheduler * cshed = new Coro_Scheduler();
  for(int i = 0; i < coro_num;  ++i){
    cshed->coroutine_create([cshed,i]{
      int x = switch_time;
      while(x--){
        double w = 1.1;
        std::cout << " hhelo" << w << std::endl;
        //std::cout << i << " " << x << std::endl;
        cshed->coroutine_yeild();
      }
    });
  }
  auto s = time();
  cshed->await();
  auto e = time();
  std::cout << "fcontext-" << coro_num << " : "
    << diff(s,e)/(coro_num*switch_time) << std::endl;
  return 0;
}
