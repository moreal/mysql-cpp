#ifndef OUTPUTBINDER_HPP_
#define OUTPUTBINDER_HPP_

#include <cassert>
#include <cstdint>
#include <cstring>
#include <mysql/mysql.h>

#include <boost/lexical_cast.hpp>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "MySqlException.hpp"

/**
 * Saves the results from the SQL query into the vector of tuples.
 */
template <typename... Args>
void setResults(
    MYSQL_STMT* const statement,
    std::vector<std::tuple<Args...>>* const results);

// The base type of the pointer MYSQL_BIND.length
typedef typename std::remove_reference<decltype(*std::declval<
    // This expression should yield a pointer to unsigned integral type
    typename std::remove_reference<decltype(
        std::declval<MYSQL_BIND>().length
    )>::type
>())>::type mysql_bind_length_t;

namespace OutputBinderPrivate {

/**
 * Helper functions that aren't template dependent for setResults. Just trying
 * to move as much of the compilable logic of this header out as I can.
 */
/// @{
void throwIfArgumentCountWrong(size_t expectedSize, MYSQL_STMT* statement);
int bindAndExecuteStatement(
    std::vector<MYSQL_BIND>* parameters,
    MYSQL_STMT* statement);
void throwIfFetchError(int fetchStatus, MYSQL_STMT* statement);
void refetchTruncatedColumns(
    MYSQL_STMT* const statement,
    std::vector<MYSQL_BIND>* const parameters,
    std::vector<std::vector<char>>* const buffers,
    std::vector<mysql_bind_length_t>* const lengths);
/// @}

static const char NULL_VALUE_ERROR_MESSAGE[] = \
    "Null value encountered with non-shared_ptr output type";

template<int I> struct int_ {};  // Compile-time counter

template<typename Tuple, int I>
void setResultTuple(
    Tuple* const tuple,
    const std::vector<MYSQL_BIND>& mysqlBindParameters,
    int_<I>);
template<typename Tuple>
void setResultTuple(
    Tuple* const tuple,
    const std::vector<MYSQL_BIND>& mysqlBindParameters,
    int_<-1>);
// Keep this in a templated class instead of just defining function overloads
// so that we don't get unused function warnings
template <typename T>
class OutputBinderResultSetter {
    public:
        /**
         * Default setter for non-specialized types using Boost lexical_cast.
         */
        static void setResult(
            T* const value,
            const MYSQL_BIND& bind);
};
template<typename T>
class OutputBinderResultSetter<std::shared_ptr<T>> {
    public:
        static void setResult(
            std::shared_ptr<T>* const value,
            const MYSQL_BIND& bind);
};
template<typename T>
class OutputBinderResultSetter<T*> {
    public:
        static void setResult(T** const, const MYSQL_BIND&);
};


template<typename Tuple, int I>
void bindParameters(
    const Tuple& tuple,  // We only need this so we can access the element types
    std::vector<MYSQL_BIND>* const mysqlBindParameters,
    std::vector<std::vector<char>>* const buffers,
    std::vector<my_bool> const nullFlags,
    int_<I>
);
template<typename Tuple>
void bindParameters(
    const Tuple& tuple,  // We only need this so we can access the element types
    std::vector<MYSQL_BIND>* const,
    std::vector<std::vector<char>>* const,
    std::vector<my_bool> const,
    int_<-1>
);
// Keep this in a templated class instead of just defining function overloads
// so that we don't get unused function warnings
template <typename T>
class OutputBinderParameterSetter {
    public:
        /**
         * Default setter for non-specialized types using Boost lexical_cast. If
         * the type doesn't have a specialization, just set it to the string type.
         * MySQL will convert the value to a string and we'll use Boost
         * lexical_cast to convert it back later.
         */
        static void setParameter(
            MYSQL_BIND* const bind,
            std::vector<char>* const buffer,
            my_bool* const isNullFlag);
};
template<typename T>
class OutputBinderParameterSetter<std::shared_ptr<T>> {
    public:
        static void setParameter(
            MYSQL_BIND* const bind,
            std::vector<char>* const buffer,
            my_bool* const isNullFlag);
};
template<typename T>
class OutputBinderParameterSetter<T*> {
    public:
        static void setParameter(
            MYSQL_BIND* const,
            std::vector<char>* const,
            my_bool* const);
};


template<typename Tuple, int I>
void setResultTuple(
    Tuple* const tuple,
    const std::vector<MYSQL_BIND>& outputParameters,
    int_<I>
) {
    OutputBinderResultSetter<
        typename std::tuple_element<I, Tuple>::type
    > setter;
    setter.setResult(&(std::get<I>(*tuple)), outputParameters.at(I));
    setResultTuple(
        tuple,
        outputParameters,
        int_<I - 1>{});
}


template<typename Tuple>
void setResultTuple(
    Tuple* const,
    const std::vector<MYSQL_BIND>&,
    int_<-1>
) {
}


template<typename Tuple>
void bindParameters(
    const Tuple&,  // We only need this so we can access the element types
    std::vector<MYSQL_BIND>* const,
    std::vector<std::vector<char>>* const,
    std::vector<my_bool>* const,
    int_<-1>
) {
}


template<typename Tuple, int I>
void bindParameters(
    const Tuple& tuple,  // We only need this so we can access the element types
    std::vector<MYSQL_BIND>* const mysqlBindParameters,
    std::vector<std::vector<char>>* const buffers,
    std::vector<my_bool>* const nullFlags,
    int_<I>
) {
    OutputBinderParameterSetter<
        typename std::tuple_element<I, Tuple>::type
    > setter;
    setter.setParameter(
        &mysqlBindParameters->at(I),
        &buffers->at(I),
        &nullFlags->at(I));
    bindParameters(
        tuple,
        mysqlBindParameters,
        buffers,
        nullFlags,
        int_<I - 1>());
}


template <typename T>
void OutputBinderResultSetter<T>::setResult(
    T* const value,
    const MYSQL_BIND& bind
) {
    // Null values only be encountered with shared_ptr output variables, and
    // those are handled in a partial template specialization, so if we see
    // one here, it's unexpected and an error
    if (*bind.is_null) {
        throw MySqlException(NULL_VALUE_ERROR_MESSAGE);
    }
    *value = boost::lexical_cast<T>(static_cast<char*>(bind.buffer));
}
// **********************************************************
// Partial specialization for shared_ptr types for setResult
// **********************************************************
template<typename T>
void OutputBinderResultSetter<std::shared_ptr<T>>::setResult(
    std::shared_ptr<T>* const value,
    const MYSQL_BIND& bind
) {
    if (*bind.is_null) {
        // Remove object (if any)
        value->reset();
    } else {
        // Fall through to the non-shared_ptr version
        // TODO(bskari|2013-03-13) We shouldn't need to allocate a new object,
        // send it to a non-shared_ptr instance of setResult, and then make a
        // copy of it. Refactor this delegation stuff out so that falling
        // through is cleaner.
        T newObject;
        OutputBinderResultSetter<T> setter;
        setter.setResult(&newObject, bind);
        *value = std::make_shared<T>(newObject);
    }
}
// *******************************************************
// Partial specialization for pointer types for setResult
// *******************************************************
template<typename T>
void OutputBinderResultSetter<T*>::setResult(T** const, const MYSQL_BIND&) {
    static_assert(
        // C++ guarantees that the sizeof any type >= 0, so this will always
        // give a compile time error
        sizeof(T) < 0,
        "Raw pointers are not supported; use std::shared_ptr instead");
}
// ***********************************
// Full specializations for setResult
// ***********************************
#ifndef OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION
#define OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(type) \
template <> \
class OutputBinderResultSetter<type> { \
    public: \
        void setResult( \
            type* const value, \
            const MYSQL_BIND& bind \
        ) { \
            if (*bind.is_null) { \
                throw MySqlException(NULL_VALUE_ERROR_MESSAGE); \
            } \
            *value = *static_cast<const decltype(value)>(bind.buffer); \
        } \
};
#endif
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(int8_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(uint8_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(int16_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(uint16_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(int32_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(uint32_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(int64_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(uint64_t)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(float)
OUTPUT_BINDER_ELEMENT_SETTER_SPECIALIZATION(double)
template<>
class OutputBinderResultSetter<std::string> {
    public:
        void setResult(
            std::string* const value,
            const MYSQL_BIND& bind
        ) {
            if (*bind.is_null) {
                throw MySqlException(NULL_VALUE_ERROR_MESSAGE);
            }
            // Strings have an operator= for char*, so we can skip the
            // lexical_cast and just call this directly
            *value = static_cast<const char*>(bind.buffer);
        }
};


template <typename T>
void OutputBinderParameterSetter<T>::setParameter(
    MYSQL_BIND* const bind,
    std::vector<char>* const buffer,
    my_bool* const isNullFlag
) {
    bind->buffer_type = MYSQL_TYPE_STRING;
    if (0 == buffer->size()) {
        // This seems like a reasonable default. If the buffer is
        // non-empty, then it's probably been expanded to fit accomodate
        // some truncated data, so don't mess with it.
        buffer->resize(20);
    }
    bind->buffer = buffer->data();
    bind->is_null = isNullFlag;
    bind->buffer_length = buffer->size();
}
// ************************************************************
// Partial specialization for shared_ptr types for setParameter
// ************************************************************
template<typename T>
void OutputBinderParameterSetter<std::shared_ptr<T>>::setParameter(
    MYSQL_BIND* const bind,
    std::vector<char>* const buffer,
    my_bool* const isNullFlag
) {
    // Just forward to the full specialization
    OutputBinderParameterSetter<T> setter;
    setter.setParameter(bind, buffer, isNullFlag);
}
// *********************************************************
// Partial specialization for pointer types for setParameter
// *********************************************************
template<typename T>
void OutputBinderParameterSetter<T*>::setParameter(
    MYSQL_BIND* const,
    std::vector<char>* const,
    my_bool* const
) {
    static_assert(
        // C++ guarantees that the sizeof any type >= 0, so this will always
        // give a compile time error
        sizeof(T) < 0,
        "Raw pointers are not suppoorted; use std::shared_ptr instead");
}
// *************************************
// Full specializations for setParameter
// *************************************
#ifndef OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION
#define OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(type, mysqlType, isUnsigned) \
template <> \
struct OutputBinderParameterSetter<type> { \
    public: \
        static void setParameter( \
            MYSQL_BIND* const bind, \
            std::vector<char>* const buffer, \
            my_bool* const isNullFlag \
        ) { \
            bind->buffer_type = mysqlType; \
            buffer->resize(sizeof(type)); \
            bind->buffer = buffer->data(); \
            bind->is_null = isNullFlag; \
            bind->is_unsigned = isUnsigned; \
        } \
};
#endif
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(int8_t,   MYSQL_TYPE_TINY,     0)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(uint8_t,  MYSQL_TYPE_TINY,     1)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(int16_t,  MYSQL_TYPE_SHORT,    0)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(uint16_t, MYSQL_TYPE_SHORT,    1)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(int32_t,  MYSQL_TYPE_LONG,     0)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(uint32_t, MYSQL_TYPE_LONG,     1)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(int64_t,  MYSQL_TYPE_LONGLONG, 0)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(uint64_t, MYSQL_TYPE_LONGLONG, 1)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(float,    MYSQL_TYPE_FLOAT,    0)
OUTPUT_BINDER_PARAMETER_SETTER_SPECIALIZATION(double,   MYSQL_TYPE_DOUBLE,   0)

}  // End anonymous namespace


template <typename... Args>
void setResults(
    MYSQL_STMT* const statement,
    std::vector<std::tuple<Args...>>* const results
) {
    OutputBinderPrivate::throwIfArgumentCountWrong(sizeof...(Args), statement);
    const size_t fieldCount = mysql_stmt_field_count(statement);

    std::vector<MYSQL_BIND> parameters(fieldCount);
    std::vector<std::vector<char>> buffers(fieldCount);
    std::vector<mysql_bind_length_t> lengths(fieldCount);
    std::vector<my_bool> nullFlags(fieldCount);

    // bindParameters needs to know the type of the tuples, and it does this by
    // taking an example tuple, so just create a dummy
    // TODO(bskari|2013-03-17) There has to be a better way than this
    std::tuple<Args...> unused;
    bindParameters(
        unused,
        &parameters,
        &buffers,
        &nullFlags,
        OutputBinderPrivate::int_<sizeof...(Args) - 1>{});

    for (size_t i = 0; i < fieldCount; ++i) {
        // This doesn't need to be set on every type, but it won't hurt
        // anything, and it will make the OutputBinderParameterSetter
        // specializations simpler
        parameters.at(i).length = &lengths.at(i);
    }

    int fetchStatus = OutputBinderPrivate::bindAndExecuteStatement(
        &parameters,
        statement);

    while (0 == fetchStatus || MYSQL_DATA_TRUNCATED == fetchStatus) {
        if (MYSQL_DATA_TRUNCATED == fetchStatus) {
            OutputBinderPrivate::refetchTruncatedColumns(
                statement,
                &parameters,
                &buffers,
                &lengths);
        }

        std::tuple<Args...> rowTuple;
        try {
            setResultTuple(
                &rowTuple,
                parameters,
                OutputBinderPrivate::int_<sizeof...(Args) - 1>{});
        } catch (...) {
            mysql_stmt_close(statement);
            throw;
        }

        results->push_back(rowTuple);
        fetchStatus = mysql_stmt_fetch(statement);
    }

    OutputBinderPrivate::throwIfFetchError(fetchStatus, statement);
}


#endif  // OUTPUTBINDER_HPP_
