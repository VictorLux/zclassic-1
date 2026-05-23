/* lib/framework/include/framework/condition.h
 *
 * Condition shape - auto-healing primitive.
 * This is a stub created by wt3 to establish the include path.
 * Real implementation shipped by wt2 in branch wt2/phase0-condition-engine.
 * See docs/FRAMEWORK.md section 3.6.
 */
#ifndef ZCL_FRAMEWORK_CONDITION_H
#define ZCL_FRAMEWORK_CONDITION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration - full struct in wt2's implementation. */
struct condition;
void condition_register(const struct condition *cond);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_FRAMEWORK_CONDITION_H */
