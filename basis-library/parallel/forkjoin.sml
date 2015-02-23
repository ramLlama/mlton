structure MLtonParallelForkJoin :> MLTON_PARALLEL_FORKJOIN =
struct

  structure B = MLtonParallelBasic
  structure HM = MLtonHM
  structure HH = HM.HierarchicalHeap
  structure I = MLtonParallelInternal
  structure V = MLtonParallelSyncVarCapture

  exception ShouldNotHappen

  datatype 'a result =
     Finished of 'a * HH.t
   | Raised of exn * HH.t

  (* RAM_NOTE: How to handle exceptions and heaps? *)
  local
      fun evaluateFunction f (exceptionHandler: unit -> unit) =
          let
              (* RAM_NOTE: I need an uncounted enter/exit heap *)
              val () = HM.exitGlobalHeap ()
              val result = (SOME(f ()), NONE)
                           (* SPOONHOWER_NOTE Do we need to execute g in the case where f raises? *)
                           handle e => (exceptionHandler ();
                                        (NONE, SOME(e)))
              val () = HM.enterGlobalHeap ()
          in
              case result
               of (SOME(r), NONE) => r
                | (NONE, SOME(e)) => raise e
                | _ => raise ShouldNotHappen
          end
  in
      fun fork (f, g) =
          let
              val () = HM.enterGlobalHeap ()
              (* make sure a hh is set *)
              val hh = HH.get ()
              val level = HH.getLevel hh
              (* Make sure calling thread is set to use hierarchical heaps *)
              val () = HH.useHierarchicalHeap ()

              (* Used to hold the result of the right-hand side in the case where
          that code is executed in parallel. Should be on hierarchical heap as
          it points to HH data *)
              val inGlobalHeapCounter = HM.explicitExitGlobalHeap ()
              val var = V.empty ()
              val () = HM.explicitEnterGlobalHeap inGlobalHeapCounter
              (* Closure used to run the right-hand side... but only in the case
          where that code is run in parallel. *)
              fun rightside () =
                  let
                      val hh = HH.get ()
                  in
                      V.write (var,
                               Finished (g (), hh)
                               handle e => Raised (e, hh));
                      B.return ()
                  end

              (* Increment level for chunks allocated by 'f' and 'g' *)
              val () = HH.setLevel (hh, level + 1)

              (* Offer the right side to any processor that wants it *)
              val t = B.addRight (rightside, level + 1)(* might suspend *)

              (* Run the left side in the hierarchical heap *)
              val a = evaluateFunction f (fn () => (ignore (B.remove t);
                                                    B.yield ()))

              (* Try to retract our offer -- if successful, run the right side
          ourselves. *)
              val b =
                  if B.remove t
                  then
                      (*
                       * no need to yield since we expect this work to be the
                       * next thing in the queue
                       *)
                      evaluateFunction g (fn () => B.yield ())
                  else
                      (* must have been stolen, so I have a heap to merge *)
                      case V.read var
                       of (_, Finished (b, childHH)) =>
                          (HH.mergeIntoParent childHH;
                           b)
                        | (_, Raised (e, childHH)) =>
                          (HH.mergeIntoParent childHH;
                           B.yield ();
                           raise e)

              (*
               * At this point, g is done and merged, as if it was performed
               * serially
               *)
              val () = HH.promoteChunks hh


              (* Reset level *)
              val () = HH.setLevel (hh, level)
          in
              B.yield ();
              (a, b)
          end
  end

  local
      fun doReduce maxSeq f g u n =
          let
              val () = if maxSeq < 1 then raise B.Parallel "maxSeq must be at least 1" else ()

              fun wrap i l () =
                  if l <= maxSeq then
                      let
                          val stop = i + l
                          fun loop j v = if j = stop then v
                                         else loop (j + 1) (f (v, g j))
                      in
                          loop i u
                      end
                  else
                      let
                          val l' = l div 2
                      in
                          f (fork (wrap i l',
                                   wrap (i + l') (l - l')))
                      end
          in
              wrap 0 n ()
          end
  in
      fun reduce maxSeq f g u n =
          let
              val _ = HM.enterGlobalHeap ()
              val result = doReduce maxSeq f g u n
              val _ = HM.exitGlobalHeap ()
          in
              result
          end
  end

  local
      fun doReduce' maxSeq (g : int -> unit) n =
          let
              val () = if maxSeq < 1 then raise B.Parallel "maxSeq must be at least 1" else ()

              fun wrap i l () =
                  if l <= maxSeq then
                      let
                          val stop = i + l
                          fun loop j = if j = stop then ()
                                       else (g j; loop (j + 1))
                      in
                          loop i
                      end
                  else
                      let
                          val l' = l div 2
                      in
                          ignore (fork (wrap i l',
                                        wrap (i + l') (l - l')))
                      end
          in
              wrap 0 n ()
          end
  in
      fun reduce' maxSeq g n =
          let
              val _ = HM.enterGlobalHeap ()
              val result = doReduce' maxSeq g n
              val _ = HM.exitGlobalHeap ()
          in
              result
          end
  end

end
