structure Mailboxes :> MAILBOXES =
struct

  exception Mailboxes

  val myMutexLock   = _import "Parallel_myMutexLock"   runtime private : int -> unit;
  val myMutexUnlock = _import "Parallel_myMutexUnlock" runtime private : int -> unit;
  val myCondWait    = _import "Parallel_myCondWait"    runtime private : int -> unit;
  val myCondSignal  = _import "Parallel_myCondSignal"  runtime private : int -> unit;

  val P = MLton.Parallel.numberOfProcessors
  val vcas = MLton.Parallel.compareAndSwap
  fun cas (r, old, new) = vcas r (old, new) = old

  type 'a mailbox = {flag : int ref, mail : 'a ref}
  type 'a t = 'a mailbox vector

  val MAIL_WAITING = 0
  val MAIL_RECEIVING = 1

  fun new d =
    Vector.tabulate (P, fn _ => {flag = ref MAIL_WAITING, mail = ref d})

  (* fun getMail mailboxes p =
    let val {flag, mail} = Vector.sub (mailboxes, p)
    in if !flag = MAIL_RECEIVING
       then (flag := MAIL_WAITING; !mail)
       else getMail mailboxes p
    end

  fun sendMail mailboxes (p, m) =
    let val {flag, mail} = Vector.sub (mailboxes, p)
    in ( mail := m
       ; if cas (flag, MAIL_WAITING, MAIL_RECEIVING) then ()
         else raise Mailboxes
       )
    end *)
  val YIELD_CNT = 1
  fun modY x = if x >= YIELD_CNT then x - YIELD_CNT else x
  
  fun getMail mailboxes p =
    let
      val {flag, mail} = Vector.sub (mailboxes, p)
      fun loop i = 
        if !flag = MAIL_RECEIVING
        then (flag := MAIL_WAITING)
        else 
          let val _ = if i >= (YIELD_CNT-1) then myCondWait(p) else ()
          in loop (modY(i+1))
          end
    in
      (
       myMutexLock  (p);
       loop (0);
       myMutexUnlock(p);
       !mail
      )
    end

  fun sendMail mailboxes (p, m) =
    let
      val {flag, mail} = Vector.sub (mailboxes, p)
      val _ = myMutexLock(p)
    in ( mail := m
       ; if cas (flag, MAIL_WAITING, MAIL_RECEIVING) then (
          myMutexUnlock(p);
          myCondSignal (p)
         )
         else raise Mailboxes
       )
    end

end