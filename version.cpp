const char *get_qaac_version()
{
#ifdef REFALAC
    return "0.25";
#else
    return "1.14";
#endif
}
