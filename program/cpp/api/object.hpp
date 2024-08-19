#pragma once
#include "variant.hpp"
#include <functional>

struct Object {
	/// @brief Construct an Object object from an allowed global object.
	Object(const std::string &name);

	/// @brief Construct an Object object from an existing in-scope Object object.
	/// @param addr The address of the Object object.
	Object(uint64_t addr) : m_address{addr} {}

	// Call a method on the node.
	// @param method The method to call.
	// @param deferred If true, the method will be called next frame.
	// @param args The arguments to pass to the method.
	// @return The return value of the method.
	Variant callv(const std::string &method, bool deferred, const Variant *argv, unsigned argc);

	template <typename... Args>
	Variant call(const std::string &method, Args... args);

	template <typename... Args>
	Variant operator () (const std::string &method, Args... args);

	template <typename... Args>
	Variant call_deferred(const std::string &method, Args... args);

	/// @brief Get a list of methods available on the object.
	/// @return A list of method names.
	std::vector<std::string> get_method_list() const;

	// Get a property of the node.
	// @param name The name of the property.
	// @return The value of the property.
	Variant get(const std::string &name) const;

	// Set a property of the node.
	// @param name The name of the property.
	// @param value The value to set the property to.
	void set(const std::string &name, const Variant &value);

	// Get a list of properties available on the object.
	// @return A list of property names.
	std::vector<std::string> get_property_list() const;

	// Connect a signal to a method on another object.
	// @param signal The signal to connect.
	// @param target The object to connect to.
	// @param method The method to call when the signal is emitted.
	void connect(Object target, const std::string &signal, const std::string &method);
	void connect(const std::string &signal, const std::string &method);

	// Disconnect a signal from a method on another object.
	// @param signal The signal to disconnect.
	// @param target The object to disconnect from.
	// @param method The method to disconnect.
	void disconnect(Object target, const std::string &signal, const std::string &method);
	void disconnect(const std::string &signal, const std::string &method);

	// Get a list of signals available on the object.
	// @return A list of signal names.
	std::vector<std::string> get_signal_list() const;

	// Get the object identifier.
	uint64_t address() const { return m_address; }

	// Check if the node is valid.
	bool is_valid() const { return m_address != 0; }

protected:
	uint64_t m_address;
};

inline Object Variant::as_object() const {
	if (get_type() == Variant::OBJECT)
		return Object{uintptr_t(v.i)};
	api_throw("std::bad_cast", "Variant is not an Object", this);
}

template <typename... Args>
inline Variant Object::call(const std::string &method, Args... args) {
	Variant argv[] = {args...};
	return callv(method, false, argv, sizeof...(Args));
}

template <typename... Args>
inline Variant Object::operator () (const std::string &method, Args... args) {
	return call(method, args...);
}

template <typename... Args>
inline Variant Object::call_deferred(const std::string &method, Args... args) {
	Variant argv[] = {args...};
	return callv(method, true, argv, sizeof...(Args));
}

inline void Object::connect(const std::string &signal, const std::string &method) {
	this->connect(*this, signal, method);
}
inline void Object::disconnect(const std::string &signal, const std::string &method) {
	this->disconnect(*this, signal, method);
}
