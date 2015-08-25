provider asd {
   probe trans__demarshal(uint64_t, uint64_t);
   probe trans__prepare(uint64_t, uint64_t, uint64_t);
   probe query__starting(uint64_t, uint64_t);
   probe query__qtrsetup_starting(uint64_t, uint64_t);
   probe query__qtrsetup_finished(uint64_t, uint64_t);
   probe query__init(uint64_t, uint64_t);
   probe query__done(uint64_t, uint64_t, uint64_t);
   probe query__trans_done(uint64_t, uint64_t, uint64_t);
   probe query__qtr_alloc(uint64_t, uint64_t, uint64_t);
   probe query__qtr_free(uint64_t, uint64_t, uint64_t);
   probe query__ioreq_starting(uint64_t, uint64_t);
   probe query__ioreq_finished(uint64_t, uint64_t);
   probe query__io_starting(uint64_t, uint64_t);
   probe query__io_notmatch(uint64_t, uint64_t);
   probe query__io_error(uint64_t, uint64_t);
   probe query__io_finished(uint64_t, uint64_t);
   probe query__netio_starting(uint64_t, uint64_t);
   probe query__netio_finished(uint64_t, uint64_t);
   probe query__addfin(uint64_t, uint64_t);
   probe query__sendpacket_starting(uint64_t, uint32_t, uint32_t);
   probe query__sendpacket_continue(uint64_t, uint32_t);
   probe query__sendpacket_finished(uint64_t);
   probe sindex__msgrange_starting(uint64_t, uint64_t);
   probe sindex__msgrange_finished(uint64_t, uint64_t);
};